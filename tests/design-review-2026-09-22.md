> 2026-09-25 目录整理：本文保留历史运行命令、次数及哈希。测试脚本现位于
> tests/scripts，build 中的手写探针/脚本现位于 tests/archive；
> 旧路径到新路径见 [迁移索引](archive/README.md)。日志仍在 build。
> 新增注释改变了源码哈希，本文历史压力结果不代表整理后重新运行的结果。

# PauseBranch 设计审查与测试记录（2026-09-22）

审查基线为 [pause-branch-design.md](../pause-branch-design.md)；`pause.hpp` 仍为 0 字节。
没有把强依赖外部库列为问题，也没有实现或修改 Pause 的生产 API、状态机或设计文档。
本轮测试验证设计模型、固定 stdexec 和测试专用的生命周期协议探针。

**2026-09-23 后续决策：** 用户确认保留恢复取消要求，将环境转发、类型擦除保留
token 及恢复取消明确为实现者的集成义务，已补入设计第 3.4、6.4 和 14.3 节。
下文保留 2026-09-22 的审查记录与当时建议；其中优先放宽验收的建议已被该决策
取代。旧 `exec::task` 的事实测试继续作为不兼容对照，不代表已满足 Pause 契约。

## 审查发现

1. **P2：恢复取消的验收条件超过固定默认 task 的实际保证。**

   设计第 3.4 节（229 行）和 6.4 节（610 行）要求不把调度阶段变为不可取消；
   第 14.3 节（1434 行）无条件要求“ResumedPass 后、schedule 执行前请求 stop，
   最终 stopped 撤销责任”。但默认 `exec::task` 暴露的 current scheduler 在类型
   擦除时没有保留 `get_stop_token` 查询，底层 scheduler 实际观察 `never_stop_token`。

   固定源码调用链为 `exec/task.hpp:49–53` → `exec/any_sender_of.hpp:597–611`
   和 `405–420` → `stdexec/__detail/__run_loop.hpp:134–136`。raw 暂停 callback
   仍可取得 task 环境里的 `inplace_stop_token`；丢失发生在恢复 scheduler 内部。

   事实测试见 [held_task / test_optional_task_stop_visibility](stdexec_task_lifetime_test.cpp)：
   显式使用 run_loop 与 starts_on，等待 inner 已启动，再 request_stop 并放行外部
   sender；task 内断言原 token 已停止，随后显式 `schedule(get_start_scheduler(...))`
   仍正常返回 42。对照组在具体 starts_on 调度前预取消，正确以 stopped 退出。
   这不是用 sleep 碰概率，也不是真实 Pause 的失败复现。

   第 6.4 节已经允许结果取决于 scheduler，并且不保证 stop 后不执行任何业务，
   所以此发现不证明完成权或计数算法有错。问题在于第 14.3 节验收没有限定环境。

   推荐顺序：① 明确 Pause 不主动覆写 token，不代表 scheduler 内部保留 token；
   将最终 stopped 验收限定到支持取消的 scheduler，保留默认 task 的事实测试。
   ② 若产品必须保证这段调度接受取消，再单独设计保留取消的 task context/scheduler。
   后者会改变集成方案，不能只靠 scheduler concept 检查，也不应在本次审查中擅自实现。

2. **P3：系统锁失败的异常策略与控制 API 伪代码不一致。**

   第 11.3 节（1321 行）声明系统锁失效 fail-fast；第 1 节只把 `lock(mutex)`
   定义为普通 RAII 加锁。`request_pause()`（411 行）和 `resume()`（434 行）
   没有 noexcept 或 catch。若直接用 `std::lock_guard` 实现，`mutex::lock()`
   的 `std::system_error` 可以逃逸。

   异常发生在状态修改、waiter claim 之前；这不是正常并发下的状态损坏或死锁。
   推荐明确统一加锁辅助函数的失败终止语义，以保持公共签名；备选是在审查所有
   异常路径后将相应控制 API 明确为 noexcept。此项基于静态审查，未注入 OS 锁失败。

在文档的结构化使用约束内，未确认新的重复完成、提前收敛、重复撤销责任或父节点
提前退役缺陷。特别核对了：fork 的责任转交只提交一次；直接 child 计数不包含孙辈；
取消先撤销责任而不退役 record；最后 child 退役不自动激活 parent；Resume 的全批
注销先于交付；terminal completion 后不访问可能已经销毁的 operation。

文档第 9.3 节已承认的风险仍真实存在：普通 sender 的 stopped 被外层吞成 value，
同时 branch frame 保留到 join，会形成 Collecting 循环等待。本轮真实 task 测试
验证嵌套 frame 保留、文档 wrapper 提前释放及 child-before-parent 顺序；反例由
外部协调者完成测试 sibling 后安全清理。未将“库 frame 保留 + 模型责任”冒称为
已经运行真实 Pause 的死锁复现，也未将文档已禁止的安排重复报为新设计漏洞。

## 测试方法与范围

模型每个事件后独立重建参与数、pending、waiter、直接 child 数、祖先链和完成次数；
预期拒绝的 API 调用检查状态不变。7 种故意缺陷必须被 oracle 拒绝，包括提前重新
激活 parent 和取消时提前减少 parent child 数。模型是串行化逻辑事件的性质测试，
不是 C++ 数据竞争检测器。

新增 173 个完整嵌套定向场景：165 个树矩阵场景，加 8 个深层 parent 重入与 Resume
顺序场景。树矩阵为链深度 1/2/4/8、满二叉深度 1/2/4、不对称宽度树深度 1/2/4/8，
组合 Open/Collecting/Paused 下退役和 5 种叶子退出分布。每场景验证实际生成深度，
最终排空 records、operations、waiters、批次、live 和 pending；峰值 32 records。
没有声称测试了 8 层满二叉树。

真实 task 使用所有权哨兵模拟 fork 的父子关系：每层两个直接 child，3/5 层链状
展开和 3 层满二叉树均覆盖 value、error、stopped 保留 frame、wrapper 提前释放四条
路径。满二叉的非 value 路径允许取消使部分子树未启动，按实际生成的树重建直接
child 计数及祖先关系；不能盲目假定都创建了 15 个节点。哨兵不实现 Pause 责任或暂停屏障。

7 类钩子用例使用固定头文件的构建副本，正式依赖不变：

- 注册可见、构造未返回时取消，只记录 CancelRequested。
- 构造期间已选中 callback，延迟到 start 分别进入 CompletingValue/Waiting 才运行。
- callback 已选中、函数未进入时 Resume 注销，确定进入库的等待循环后才放行。
- start 的 Waiting 发布后暂停尾部，value/stopped 完成分别销毁整个协议探针。
- completion 发布后另一线程析构，实际等待 callback 尾部返回；协调者独立放行。

所有并发正确性交错使用 latch/barrier，未用 sleep。CTest 每组总超时 60 秒；本轮
ASan 模型长跑设 1800 秒总超时，其余压力进程设 300 秒总超时。

## 运行结果

环境：Windows x64，MSVC **19.44.35225.0**，C++23/latest，固定 stdexec
`f0e8ae6fdc6c188389b146bb854b80d399724b04`。ASan 使用 `/O2 /MD /Zi /fsanitize=address`，
与 Debug 的 `/RTC1` 构建分离。复现构建命令见 [README](README.md)。

| 验证 | 结果 |
| --- | --- |
| MSVC Debug 配置、构建及 CTest | 4/4 通过，12.73 秒；最后新增满二叉用例后，受影响 task 测试重跑 1/1 通过，1.43 秒 |
| ASan 原 callback 测试 | 5 个单次用例 + 7 类并发用例 × 10,000 = 70,005 个用例通过 |
| ASan 新增确定性钩子 | 7 类 × 10,000 = 70,000 个用例通过 |
| ASan task 生命周期/调度事实测试 | 最终 25 类 × 5,000 = **125,000** 个用例通过，95.95 秒 |
| ASan 模型定向与故障注入 | 193 个定向通过，7/7 故意缺陷被拒绝 |
| ASan 模型有界搜索 | 事件深度 10，最多 3 records；30,223 个状态、63,792 条转换 |
| ASan 模型随机事件 | 100 个固定种子 × 100,000 = **10,000,000** 个事件，通过；总运行 729.82 秒 |

1000 万的计数是随机模型事件，不是完整场景数：其中 **9,788,868** 次成功事件，
**211,132** 次预期 API 拒绝（同样检查状态不变），覆盖 16 类事件。成功 fork
**499,650** 次，其中嵌套 fork **391,141** 次；最大 fork 深度 **21**、最大宽度 3，
峰值 24 records。21 层出现 1 次，详细深度直方图保留在日志中，不代表每层覆盖等量。

| 随机边界事件 | 实际次数 |
| --- | --- |
| Open / Collecting 中 fork | 214,295 / 285,355 |
| Open / Collecting / Paused 中重入 | 41,250 / 104,289 / 41,133 |
| Open / Collecting / Paused 中退役 | 290,292 / 619,763 / 165,997 |
| 构造期取消 / Waiting 取消 / scheduler stopped | 807,460 / 154,883 / 29,661 |
| 深度至少 2 的取消 | 628,274 |
| 完整排空的随机 scope | 226 |

每个固定种子的事件预算耗尽时允许仍有模型记录，因此 1000 万事件不能转换成
1000 万次完整生命周期释放证明。173 个完整嵌套定向场景单独验证全部资源排空。

第一次扩展测试曾使用错误前提“token 预停止必定阻止 task body 启动”，之后又错误
假设默认 task 恢复 scheduler 必定 stopped，导致断言失败及受控测试挂起。依据源码
与确定性对照修正测试后得到上表结果。最终测试保留 token 擦除的实际 value 行为，
没有用放宽断言掩盖第 14.3 节的契约问题。

四组 ASan 测试均正常退出，stderr 日志为空，未报告内存错误。运行日志位于
[build/design-review-20260922](../build/design-review-20260922)，包括
[模型完整输出](../build/design-review-20260922/model-10m-asan.stdout.log)、
[callback 输出](../build/design-review-20260922/callback-asan.stdout.log)、
[钩子输出](../build/design-review-20260922/hook-asan.stdout.log)、
[task 输出](../build/design-review-20260922/task-asan.stdout.log)和
[源码 SHA256](../build/design-review-20260922/source-sha256.log)。

## 尚未验证的验收项

| 文档要求 | 本轮覆盖与缺口 |
| --- | --- |
| 14.1 API/收敛/epoch | 模型覆盖状态和错误优先级；真实类型限制、分配失败及公共 API 尚不可测 |
| 14.2 callback/生命周期 | 固定库与协议探针覆盖关键内部窗口；真实公开 receiver、外层组合存储、intrusive 节点尚不可测 |
| 14.3 调度恢复 | 确认默认 task 的 token 擦除事实；真实 Pause 精确 schedule 次数、不同资源恢复、编译拒绝、启动后 error death tests 尚不可测 |
| 14.4 fork/stdexec | 模型覆盖多层记账，真实库覆盖嵌套 frame 与 wrapper；真实 fork 各阶段资源失败回滚、notifier 并发与停机 join 尚不可测 |
| 14.5 工具 | 已运行 MSVC 与 ASan；未运行 TSan，未将空 pause.hpp 的头文件 smoke 构建当作多翻译单元验收 |

模型还要求旧 operation 已析构后才允许再次使用 branch，比文档“公开 operation 完成”
更保守，不能证明完成后的同步复用以及旧 callback/start 尾部的所有真实交错。随机
压力和有限状态搜索都不是无限交错的证明；ASan 未报错也不证明不存在数据竞争。

## 修改文件与影响

- `tests/pause_state_model_test.cpp`：嵌套场景、独立不变量、故障注入与实际覆盖统计。
- `tests/stdexec_task_lifetime_test.cpp`：嵌套所有权、wrapper 回归与 scheduler 取消事实。
- `tests/stdexec_stop_callback_hook_test.cpp`、`tests/instrument_stop_token.cmake`：仅测试副本插桩。
- `CMakeLists.txt`：仅在已有 `PAUSE_BUILD_DESIGN_TESTS` 选项启用时增加 hook 测试。
- `tests/README.md`、本报告：复现命令、审查发现、结果及覆盖边界。

生产实现和公共 API 未变，未新增第三方依赖，无数据或配置迁移。2026-09-22 审查时，
恢复 scheduler 是否必须保留取消传播尚待决定；2026-09-23 已决定保留该要求，
后续待完成具体集成适配及正向验证，见本文开头的决策补充。
