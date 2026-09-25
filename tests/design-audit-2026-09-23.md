> 2026-09-25 目录整理：本文保留历史运行命令、次数及哈希。测试脚本现位于
> tests/scripts，build 中的手写探针/脚本现位于 tests/archive；
> 旧路径到新路径见 [迁移索引](archive/README.md)。日志仍在 build。
> 新增注释改变了源码哈希，本文历史压力结果不代表整理后重新运行的结果。

# PauseBranch 设计审查与边界测试（2026-09-23）

本轮审查对象为当前 [设计契约](../pause-branch-design.md)，重点是逻辑、并发冲突和生命周期，不将固定外部依赖视为问题。`pause.hpp` 仍为 0 字节；本轮没有实现 Pause，也没有修改设计契约。

## 审查结论

在文档规定的使用前提下，未找到新的、可确定复现的状态机计数错误、双重完成或必然死锁。该结论来自伪代码/固定库源码审查、状态模型及依赖实验，不是生产实现的正确性证明。

核心设计在以下方面自洽：

- fork 在唯一状态锁内把一份 parent 责任原子转交为 N 份 child 责任；Collecting 时 live/pending 均净增加 N−1，中途不提交收敛。
- child 取消只撤销有效责任，不提前减少 parent 的直接 child record 数；最后 child 退役不会自动重新激活 parent。
- parent 的重新参与和到达在同一事务完成；加入已有 Paused 不改变该轮 epoch。
- SingleStep 先建立下一轮 pending，再放行旧 waiter，没有先运行后关门的窗口。恢复调度 stopped 需要外层撤责，否则确实会遗留 pending；当前设计已包含此清理。
- start、Resume 和 callback 用同一 mutex 裁决唯一完成权；callback 注册/注销、通知、receiver completion 均在锁外。构造期取消只登记标志，避免销毁正在构造的 callback。
- Resume 的双遍批次先静默整批 callback，再逐项交付；每次交付后不再访问该节点。
- stopped 先撤责，再排入不可取消的宿主 affinity 跳转；Source、stop source 和 scheduler 的使用期不以责任归零或 waiter 摘链结束。

对应设计位置：第 4–5、6.3–6.5、7.2–7.6、8、12 节。固定 `stop_token.hpp`、`__let.hpp`、`__task.hpp`、`__when_all.hpp` 的实际行为与上述约束相符。

## 需要保留的风险与未验收项

### 1. 外部 stopped 被转换为 value，可以造成收敛循环等待

这是第 9.3 节已经识别的真实组合风险，不是本轮新发现的设计矛盾。

复现场景：A 的 frame 拥有 branch，在普通外部 sender 上 stopped；A 外层使用 `stopped_as_optional`，因此 when_all 不再把这一结果当作取消。A 的 stopped frame 若保留到 join 释放，其责任仍然存在；B 到达暂停点后等待 Resume，A 退役又等待 B 完成。此时 Collecting 无法收敛，不能靠重复 Resume 解决。

当前推荐顺序：

1. 能修改 A 时，在 A 内部处理预期局部停止，正常返回明确业务结果。
2. 需要保留 A 接口时，使用第 9.3.1 节的 `as_optional_task`，在向 when_all 提交 value 前释放 A 的等待 operation/frame。
3. 允许整组取消时，保留停止传播，不在单个 child 外吞掉 stopped。

本轮重新运行了正常/异常/停止路径、3/5 层链和满二叉所有权哨兵实验。它们验证 frame 释放条件；真实 Pause 的 Collecting→Paused→Resume 仍待实现后集成验证。提前销毁未完成 operation 不是解决方案。

### 2. 当前 error 探针不验证现行 terminate 策略（高优先级验收缺口）

`stdexec_standard_task_scheduler_test.cpp` 的 `ProtocolSender::affine()`（约 275–298 行）仍保留可行性实验中的 error 调度与转发；当前设计第 6.3/6.5 节要求内部 error 直接终止进程。测试也明确输出“不包含 termination policy”。

因此 35 类调度探针通过，只支持环境、取消、调度次数和执行资源结论，不能表示当前 error 策略已经验收。

后续真实实现必须用独立进程覆盖 let 分派、schedule 构造、内部 connect 失败、本体 error 与宿主适配 error，验证业务 `let_error`/catch 不被执行，且不追加错误恢复调度。直接调用测试 receiver 的 `set_error` 不足以覆盖这些入口。

### 3. 组合器可以在 terminal CPO 内销毁前驱 operation

固定 `__let.hpp` 的 `__start_next_fn` 用 variant 切换到后继 operation，会立即销毁前驱。因此“callback 不主动 reset 自己”并不等于“callback 函数返回前对象一直存活”。设计的 terminal 尾调用要求不可省略。

本轮新增真实组合探针验证 stopped 路线：

- callback → `let_stopped` → 前驱 operation 析构/同线程自注销 → `unstoppable(run_loop schedule)` → stopped。
- callback/request_stop 栈停在外部测试闩锁时，宿主先完成 stopped，整个外层 operation 先销毁，随后放行 callback 尾部；只访问独立 Control 存储，stop source 保活至请求线程 join。

这补上了原 callback 与 scheduler 探针分离的一处缺口。它仍不包含真实 AccountingReceiver、侵入链、批次节点或 fork。raw value→恢复调度的切换、完整 Resume 批次的同步嵌套取消以及真实 frame/branch 的快速跨线程销毁仍须集成验收。

### 4. 一千万模型事件不能证明 C++ 并发与全部生命周期

`pause_state_model_test.cpp` 的模型具有独立的计数/关系重建检查及故意缺陷检测，但验证范围有限：

- `ScheduleValue`/`ScheduleStopped` 是外部输入，不追踪 stop source 身份、持久停止状态或真实执行队列，不能凭模型通过证明第 6.4 节的取消契约。真实 task/run_loop 探针另行验证排队后 stop 的行为。
- `idle()` 要求旧 operation 已销毁，比文档允许“公开 operation 完成后复用 branch”更保守。因此 completion 中立即启动下一暂停点/再次 fork，仍是待验收边界。
- 有界搜索的深度是事件深度，最多 3 个 records，不是 10 层 fork 穷举。深层树由定向场景和随机压力覆盖。
- 随机压力在固定事件预算结束时不强制排空；不能把每个随机事件或每个 seed 称为完整工作流。完整排空的嵌套场景单独计数。
- ASan 不检测数据竞争；本轮未运行 TSan。

这些限制不能通过增加重复次数消除。当前可接受结论是“已检查的模型性质和依赖行为通过”，而非“真实 Pause 并发验收完成”。

## 本轮最小测试变更

| 文件 | 变更与目的 |
| --- | --- |
| `tests/pause_state_model_test.cpp` | 新增 `nested_overlap_tests()`，25 个完整排空场景；不修改模型转移或随机生成器 |
| `tests/stdexec_callback_affine_lifetime_test.cpp` | 新增真实 callback/let_stopped/run_loop 组合的两类确定性析构顺序实验 |
| `CMakeLists.txt` | 在既有设计测试开关下注册新探针，沿用 60 秒 CTest 超时 |
| `tests/README.md` | 登记本轮测试范围、入口和报告 |
| 本报告 | 保存结论、计数口径、验收缺口及复现方法 |

25 个新增模型场景包括：4/8 层嵌套、Continuous/SingleStep、parent 重入与两个持相同 epoch 的控制方全部 6 种排列（24 个）；以及 4 层跨轮场景（1 个）：旧恢复批次仍有未交付节点，一侧已重新进入并再次 fork，已有 waiter 时继续 fork，另一侧调度 stopped 后收敛，同轮 parent 重入保持 epoch，下一轮混合 value/stopped 后全部退役。

现有测试已经覆盖 Open/Collecting 内递归构造链、二叉树、混合宽度树，并在 Open/Collecting/Paused 退役，本轮未重复添加。没有变更公共 API、配置格式、依赖版本或生产行为；不需要迁移。撤销本轮测试变更只需移除新 CMake 条目、新探针和 `nested_overlap_tests()`。

## 第 14 节验收映射

| 文档要求 | 本轮证据 | 尚缺的真实实现验收 |
| --- | --- | --- |
| 14.1 API、屏障、空轮、epoch、重入 | 模型不变量、定向/故障注入、有界/随机事件 | 真实 API、move/析构、intrusive storage、创建失败 |
| 14.2 callback 和生命周期 | 真实 stop callback、库钩子、新增 let_stopped 析构顺序探针 | 完整 raw sender/AccountingReceiver、全批次销毁与重入 |
| 14.3 调度恢复 | stdexec::task/run_loop 的 0/1 调度、原 token、排队后取消、两个资源、撤责哨兵顺序 | 真实 SingleStep pending、内部 error death tests、编译失败用例、完整公开 sender |
| 14.4 fork 与集成 | 深层模型树、旧/新 task 的 frame/直接 child 所有权哨兵 | 真实多层 PauseBranch::fork、task 创建/分配失败回滚、多取消域结合真实计数、notifier/observer 关闭 |
| 14.5 工具与依赖 | MSVC Debug、ASan、固定源码审查、哈希校验 | TSan、真实多翻译单元 Pause API 验收 |

本轮没有需要用户选择的设计变更。若进入生产实现阶段，应优先完成上述真实 operation 的生命周期与终止策略验收，再考虑扩大后端范围。

## 构建与依赖实验结果

环境为 Windows x64、MSVC 19.44.35225.0；ASan 使用 `/O2 /MD /Zi /fsanitize=address`。实际核对依赖 HEAD 为 `f0e8ae6fdc6c188389b146bb854b80d399724b04`，依赖 checkout 没有本地修改。stop-token 插桩前原始头的 SHA256 为 `243fb98e894c35cac231de20436f82c6a8de009c7178831fbc2c9c9536658bf3`，符合测试脚本校验。

| 本轮执行 | 结果 |
| --- | --- |
| 改动前 Debug CTest | 7/7 通过 |
| 最终源码 Debug CTest | 8/8 通过，24.40 秒 |
| ASan 一千万事件主压力 | 100 个种子 × 100,000 = **10,000,000**；9,788,868 成功事件 + 211,132 预期 API 拒绝；退出 0，1383.32 秒，无超时或 ASan 报告 |
| 最终模型 ASan 补充验证 | 218 个定向场景、7/7 故意缺陷被拒绝；深度 10 搜索 30,223 个状态、63,792 条转换；额外 200,000 个随机事件通过 |
| 原始 stop callback ASan | 5 个单次 + 7 类 × 10,000 = 70,005 个用例通过 |
| 确定性 callback 钩子 ASan | 7 类 × 10,000 = 70,000 个用例通过 |
| 新增 callback/let_stopped/affinity ASan | 2 类 × 10,000 = 20,000 个用例通过 |
| 旧 exec::task 生命周期 ASan（对照） | 25 类 × 5,000 = 125,000 个用例通过 |
| 新 stdexec::task API/资源 ASan | 13 类 × 5,000 = 65,000 个用例通过 |
| 新 stdexec::task 生命周期 ASan | 23 类 × 5,000 = 115,000 个用例通过 |
| 新 stdexec::task 调度探针 ASan | 35 类 × 5,000 = 175,000 个用例通过；含旧 error 路线对照，不是 terminate 验收 |

七组依赖/组合实验共 640,005 个用例，均退出 0，无 ASan 报告，stderr 文件均为空。定向场景、随机事件和依赖用例分别计数，不合并为“完整 Pause 工作流数”。

一千万事件的随机嵌套统计：

| 指标 | 实际计数 |
| --- | ---: |
| 固定 seed 基值 | 344693494597 |
| 成功 fork | 499,650 |
| 嵌套 fork（child 深度至少 2） | 391,141 |
| 最大 fork 深度 / 最大宽度 / 峰值 records | 21 / 3 / 24 |
| Open / Collecting 中 fork | 214,295 / 285,355 |
| Open / Collecting / Paused 中重入 | 41,250 / 104,289 / 41,133 |
| 深层取消 | 628,274 |
| 构造期取消 / Waiting 取消 / scheduler stopped | 807,460 / 154,883 / 29,661 |
| 随机期间观察到完整退役的 scopes | 226 |

**二进制口径说明：** 主压力在新增定向场景编辑前从基线源码重新编译并启动，先执行原有 193 个定向场景，再运行一千万随机事件。期间没有覆盖该运行中的二进制。新增 25 个场景后，最终源码另行通过 ASan 的 218 个定向场景、有界搜索和 200,000 个随机事件，以及最终 8/8 CTest；模型的 `perform`、`enabled`、`random_runs` 没有修改。不能把主压力日志中的 193 写成 218，或声称最终二进制又完整跑过一千万事件。基线源码 SHA256 `A4CF3F5237E8DB58284A15D6B4285D1D6EE1D3D3AD9648BE2D059A7B036F1388` 已在运行前保存。

原始日志保存在 [build/design-audit-20260923](../build/design-audit-20260923/)：`debug-ctest-final.log`、各 `*.stdout.log`/`*.stderr.log`、含命令/退出码/超时/耗时的 `*.result.json`。最终模型补充验证日志为 `model-final-200k-asan.stdout.log`；原件和构建/运行脚本在 `build/nested-model-review/`。源文件及二进制哈希另存 `final-source-sha256.log`、`binary-sha256.log`。

一千万事件的直接证据为 [完整输出](../build/design-audit-20260923/model-10m-asan.stdout.log)、[退出码与耗时](../build/design-audit-20260923/model-10m-asan.result.json) 和 [stderr](../build/design-audit-20260923/model-10m-asan.stderr.log)。

## 复现与后续验证

在仓库根目录执行：

```powershell
.\scripts\msvc-cmake.cmd test
.\build\design-audit-20260923\run-asan.cmd model
.\build\design-audit-20260923\run-asan.cmd contracts
.\build\design-audit-20260923\build-extra.cmd
```

以上 ASan 命令使用本轮保留的构建产物；从最终源码重新编译全部 8 组的通用命令见 [README 的 AddressSanitizer 部分](README.md#addresssanitizer)。新增探针可单独运行：

```powershell
.\build\msvc-debug\stdexec_callback_affine_lifetime_test.exe --iterations 10000
```

压力进程均有独立总超时（模型 1800 秒、原有依赖各 300 秒、新探针 120 秒）；测试交错依靠 latch/barrier，不用 sleep 证明正确性。执行包装只对本次 PowerShell 子进程设置 ExecutionPolicy，不修改系统策略。

TSan 未运行。若后续具备支持固定 stdexec 的 Linux/Clang 工具链，可尝试以下命令；该配置本轮未验证，不能报告为通过：

```sh
cmake -S . -B build/tsan -DPAUSE_BUILD_DESIGN_TESTS=ON \
  -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-O1 -g -fsanitize=thread" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
cmake --build build/tsan
ctest --test-dir build/tsan --output-on-failure
```

真实 Pause 第 14 节集成验收目前没有可执行实现，不能提供一条能完成这些缺失验收的命令。应在实现后按上方映射补齐测试；继续重复现有探针不能替代此工作。
