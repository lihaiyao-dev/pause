# Pause 实现、设计与依赖契约测试

当前测试直接覆盖 `pause.hpp`，并保留设计模型及固定 stdexec 的独立探针。
最新的 Release 极端压力、逐场景次数、覆盖边界和复现命令见
[Release 极端测试报告](pause-release-extreme-validation-2026-09-23.md)。
生产实现及依赖保持不变；新增测试不依赖 `assert`，`/DNDEBUG` 下仍检查结果。

```powershell
.\scripts\msvc-cmake.cmd test release
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts/run-pause-release-stress.ps1 -Iterations 100000
```

长压力使用独立脚本的进程超时，不受 CTest 短回归超时限制；日志包含编译命令、
源码/头文件/二进制哈希、每个测试的输出、退出码和耗时。
`-OnlyTest pause_extreme_test` 或 `-OnlyTest pause_batch_extreme_test` 可单独复现新矩阵。
TSan 脚本的 `--suite extreme` / `--suite batch` 分别选择新测试。

下文为历次记录，其中“生产头为空”等表述只描述当时状态；当前实现状态以
上述最新报告和[实现验收记录](pause-implementation-validation-2026-09-23.md)为准。

## 本次逻辑、并发与生命周期复审（2026-09-23）

按本轮要求重新完成 **10,000,000 个 ASan 模型事件**，退出 0，无 ASan 报告；
其中 9,788,868 成功事件、211,132 预期 API 拒绝。随机嵌套 fork 391,141 次，
最深 21 层；另有 64 层完整模型场景和真实 task 的 32/64 层验证通过。
本轮没有扫描栈极限，不将历史极深嵌套栈耗尽列作此次设计缺陷。

最新源码共 227 个定向模型场景；callback/affinity 生命周期探针从 2 类增加为
4 类，并在逐层 task join 尚未完成时新增直接 child/frame 保留断言。
最终 Debug CTest 10/10、7 组组件 ASan 共 660,005 例、72 个深层 task 场景通过，
另有 8 个独立 terminate 组合探针通过。主压力的基线二进制包含 218 个定向场景；
最终源码另测 227 个定向场景及 200,000 随机事件，不混淆二进制口径。

未确认使用契约内的新逻辑、并发或生命周期缺陷。第 9.3 节外部 stopped 转 value
后延迟释放 frame 的循环等待风险仍须按设计处理。完整结论、修改文件、计数、
复现命令和第 14 节未实现验收项见
[本次审查报告](design-review-10000000-2026-09-23.md)。下文保留之前的运行记录。

## 多层链式 fork 极限测试（2026-09-23）

本轮极限测试**发现深链栈耗尽，不能报告为全部通过**。普通优化构建中，
带侧支链的快速业务 error 路线在 105/106 层附近出现失败；正常纯链测至
8192 层通过，不能代表停止和析构路径同样安全。边界复测有波动，具体数据、
调试栈及建议见 [深链极限测试报告](deep-chain-audit-2026-09-23.md)。

新增深链模型和真实 task 探针后，常规 CTest 为 **10 组**；本轮 10/10 通过。
模型逐事件检查完整保留，ASan 下 128 层的 12 个完整 scope 通过，256 层触及
120 秒测试预算。真实极限进程的故障与常规回归分开记录。

```powershell
.\build\msvc-debug\pause_state_model_test.exe --chain-depth 16 --chain-only 1
.\build\msvc-debug\stdexec_deep_chain_test.exe --depth 32 --topology fork --outcome error
```

`stdexec_deep_chain_test` 支持 `chain`/`fork` 和
`value`/`error`/`stopped`/`root-stop`/`error-fast`/`stopped-fast`；无参数运行
0/1/8/16 层共 48 个场景。极限扫描脚本为 `scripts/run-chain-stress.ps1`，
用法与隔离预算见上述报告；不要将观测故障深度当成跨平台安全上限。

## 当前设计复审（2026-09-23）

本轮结论、执行记录和第 14 节验收映射见
[设计审查与边界测试报告](design-audit-2026-09-23.md)。新增 25 个完整排空的
深层 epoch/多控制方/跨轮批次场景，定向模型总数为 218；另外新增真实
callback → `let_stopped` → 不可取消 run_loop 跳转的两类析构顺序实验。
常规 CTest 现为 8 组，测试仍只覆盖设计模型及依赖契约。

本轮重新完成 **10,000,000 个 ASan 模型事件**，最大嵌套深度 **21**，
391,141 次嵌套 fork；另有 640,005 个依赖/组合用例通过，未报告 ASan 错误。
最终 CTest 8/8 通过。主压力使用新增定向场景之前的二进制；最终源码的
218 个定向场景和额外 200,000 个随机事件另行通过 ASan。详细计数、二进制
对应关系及原始日志见上述报告；这些事件不是一千万个真实 Pause 工作流。

现有 scheduler 可行性探针的 error 路线仍是调度后转发，不能用于验收当前设计的
内部 error 直接 terminate 策略。完整 Pause 的计数、侵入链、终止与生命周期
验收依然需要真实实现。下方各历史运行记录保留当时的测试数量及结论。

## 当前采用决策（2026-09-23）

用户已批准将设计基线迁移到 **`stdexec::task` + 全通道 affine 宿主适配 +
审核过的 scheduler**，首版限定已审查的 **`run_loop` + 默认 allocator** 路径。
固定 stdexec 提交不变；旧 `exec::task` 测试保留为历史行为与不兼容配置的对照。

选定适配保留直接等待时 Open 零次、正常恢复一次 Pause 调度；stopped 必要时
额外返回宿主执行资源。只有该宿主 affinity 跳转不可取消，原 Pause 恢复调度仍须
观察原 receiver token，内部 error 仍须终止。不能把结果推广到任意 scheduler
或把 stopped 虚报为 affine。

下文保留各轮实验的原始结果和当时未迁移的状态；当前设计已作出采用决定，
生产 `pause.hpp` 仍为空。真实 Pause 的实现和完整验收尚未完成，详见
[可行性报告中的采用决策](stdexec-task-feasibility-2026-09-23.md)。

采用后已更新主设计的 task、affine 伪代码、scheduler 接入、生命周期和验收要求，
工具链 smoke 改为实际运行新 task/run_loop。调度探针在原33类上新增2类顺序验证：
callback 取消、恢复排队取消时，先撤销责任哨兵，后完成宿主 stopped 跳转。
本轮 Debug CTest **7/7**、新 smoke 均通过；调度 ASan **35类 × 1,000轮 = 35,000**
通过，stderr为空。日志在 `build/task-adoption-20260923/`。
此结果不替代真实 Pause 的 SingleStep 记账及 callback 生命周期验收。

| 文件 | 检查内容 |
| --- | --- |
| `pause_state_model_test.cpp` | 状态锁事务和分离的构造、裁决、注销、交付、调度事件；独立重建计数和关系；多层 fork 完整场景、有界搜索、固定种子压力、故意缺陷检测 |
| `stdexec_stop_callback_test.cpp` | 真实 stop callback 的注册、请求、注销、自销毁；使用 mutex 完成权探针检查竞争结果 |
| `stdexec_stop_callback_hook_test.cpp` | 测试副本中的构造返回、callback 选中、注销等待钩子；7 类确定性交错及 operation 协议探针销毁 |
| `stdexec_callback_affine_lifetime_test.cpp` | 真实 callback 在 let_stopped 切换时自注销；callback/request_stop 尾部、terminal CPO 尚未返回时外层 operation 已完成并销毁；4 类确定性交错 |
| `stdexec_task_lifetime_test.cpp` | 旧 `exec::task`/when_all 的 frame 释放、兄弟取消及跨线程 start/completion 握手；3/5 层嵌套及满二叉所有权哨兵；旧默认 task 恢复 scheduler 的 token 可见性 |
| `stdexec_standard_task_api_test.cpp` | 新 `stdexec::task` 的返回值、错误/停止、环境、token 桥接、分配失败和 frame 回收；固定版类型化错误差异 |
| `stdexec_standard_task_lifetime_test.cpp` | 新 task 的23类所有权与生命周期实验，含3/5层链、3层二叉分支（正常路径满展开）、wrapper、预取消、兄弟取消及跨线程握手 |
| `stdexec_deep_chain_test.cpp` | 全树启动后的纯链/侧支链；六种结果、逐层兄弟取消、根停止、frame 释放及默认 48 个回归场景；可独立参数化测深度 |
| `stdexec_standard_task_scheduler_test.cpp` | 新 task 的实际调度计数、恢复取消、环境及执行资源；现有属性负对照与公开 affine 定制探针 |

## 常规运行

在仓库根目录执行：

```powershell
.\scripts\msvc-cmake.cmd test
```

此命令在现有 MSVC Debug 构建目录启用 `PAUSE_BUILD_DESIGN_TESTS`、构建并执行
CTest。该选项默认关闭；启用后保存在此构建目录的 CMake cache 中。每组测试有
60 秒总超时，断言在 Release 下仍然有效。

`instrument_stop_token.cmake` 校验固定头文件 SHA256 后，仅在构建目录生成
带 3 个测试钩子的副本，仅 hook executable 使用；正式依赖及其他测试不使用
该副本。依赖变更后必须重新审查插入位置，不能直接绕过哈希检查。

增加测试数量：

```powershell
.\build\msvc-debug\pause_state_model_test.exe --depth 10 --random-steps 100000 --seeds 100 --max-records 24
.\build\msvc-debug\stdexec_stop_callback_test.exe --iterations 10000
.\build\msvc-debug\stdexec_stop_callback_hook_test.exe --iterations 10000
.\build\msvc-debug\stdexec_callback_affine_lifetime_test.exe --iterations 10000
.\build\msvc-debug\stdexec_task_lifetime_test.exe --iterations 5000
```

模型输出实际探索的状态数、转换数和各类事件次数。模型参数增加时耗时和内存
也会增长；较大的压力任务建议使用下面的优化构建。`100 × 100000` 是 1000 万
个模型事件（含单列统计的预期 API 拒绝），不是 1000 万个完整工作流或真实线程
交错。完整嵌套场景、实际最大 fork 深度/宽度、各类取消和 Gate 统计另行输出。
并发实验使用 latch/barrier，不使用 sleep 作为正确性依据。

## AddressSanitizer

先完成常规配置，使固定 stdexec 源码位于已有的构建缓存中。然后在 Visual Studio
的 **Developer PowerShell（x64）** 中，从仓库根目录运行以下命令。此构建独立于
Debug 目录，避免 `/RTC1` 与 ASan 冲突。

```powershell
New-Item -ItemType Directory -Path build/msvc-asan-tests -Force | Out-Null
$testNames = @(
    'pause_state_model_test',
    'stdexec_stop_callback_test',
    'stdexec_stop_callback_hook_test',
    'stdexec_callback_affine_lifetime_test',
    'stdexec_task_lifetime_test',
    'stdexec_standard_task_api_test',
    'stdexec_standard_task_lifetime_test',
    'stdexec_deep_chain_test',
    'stdexec_standard_task_scheduler_test'
)
foreach ($testName in $testNames) {
    & cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor `
        /EHsc /MD /O2 /Zi /fsanitize=address `
        /Ibuild/msvc-debug/_deps/stdexec-src/include `
        /Ibuild/msvc-debug/_deps/stdexec-src/include/stdexec `
        /Ibuild/msvc-debug/pause-test-hooks `
        "/Febuild/msvc-asan-tests/$testName.exe" `
        "/Fobuild/msvc-asan-tests/$testName.obj" `
        "/Fdbuild/msvc-asan-tests/$testName.pdb" `
        "tests/$testName.cpp" /link /INCREMENTAL:NO
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $testName" }
}

.\build\msvc-asan-tests\pause_state_model_test.exe --depth 10 --random-steps 100000 --seeds 100 --max-records 24
.\build\msvc-asan-tests\stdexec_stop_callback_test.exe --iterations 10000
.\build\msvc-asan-tests\stdexec_stop_callback_hook_test.exe --iterations 10000
.\build\msvc-asan-tests\stdexec_callback_affine_lifetime_test.exe --iterations 10000
.\build\msvc-asan-tests\stdexec_task_lifetime_test.exe --iterations 5000
.\build\msvc-asan-tests\stdexec_standard_task_api_test.exe --iterations 5000
.\build\msvc-asan-tests\stdexec_standard_task_lifetime_test.exe --iterations 5000
.\build\msvc-asan-tests\stdexec_standard_task_scheduler_test.exe --iterations 5000
```

## 结论边界

- A 从未持有 branch 时，不会产生该 branch 的责任。生命周期实验的无哨兵用例
  是对此的对照；只有哨兵实际移入 A frame 的用例才检查它是否延迟退役。
- stopped 被转换为 value 后，A frame 的所有权可能保留到 join 释放；实验总会
  显式放行 B 完成清理，不让进程永久等待。
- 模型枚举逻辑事件，不验证 C++ 内存序，也不验证实际 intrusive 节点或析构顺序。
  随机测试和有限深度搜索不是所有交错的完整证明。
- 原 `stdexec_stop_callback_test` 的竞争测试不能确定性截停库内部窗口；新增
  hook executable 以构建副本补齐“注册可见但构造未返回”“库已选中但用户
  callback 尚未进入”，并确认注销实际进入等待循环。
- hook 测试覆盖协议探针的同步销毁、start 尾部及异线程回收，不覆盖尚未实现的
  真实 Pause receiver、组合器外层存储与 intrusive 节点的完整生命周期。
- 模型要求旧 operation 已销毁才允许复用 branch，比文档“公开 operation 完成”
  更保守；模型没有证明完成后的同步重入及旧调用栈尾部安全。
- task 的 value 握手实验可在真实 receiver 存储结果时发信号；stopped 路径只能
  定位到尝试交付。二者都检查 continuation/最终 stopped 是否晚于 start 尾部，
  但不能保证线程已进入库内部自旋，属于重复交错实验，不是该内部窗口的证明。
- ASan 检查内存错误，不是数据竞争检测器。本轮未运行 TSan。
- 真实 Pause 的完整 sender、精确调度次数、分配失败回滚、内部 error 终止、
  编译期环境拒绝等验收，仍需 `pause.hpp` 实现后执行。
- 固定默认 `exec::task` 的类型擦除 scheduler 不向底层转发 stop token。新增测试确认
  token 已停止时显式恢复调度仍可 value；测试通过并不表示文档第 14.3 节的
  stopped 要求已满足。2026-09-23 已决定保留该要求，环境转发和恢复取消由实现者
  负责；当前已选用上方的新 task 与宿主适配方案，该旧测试继续作为不兼容对照。
  新组件的正向探针仍不能替代真实 Pause 的恢复取消验收。
  具体差异及后续决策见[审查报告](design-review-2026-09-22.md)。

## 扩展前运行记录（2026-09-22，保留历史）

环境：Windows x64，MSVC 19.44.35225，stdexec 固定提交
`f0e8ae6fdc6c188389b146bb854b80d399724b04`。ASan 使用上面的 `/O2 /MD`
独立构建命令。最终结果如下；ASan 三组均未报告内存错误。

| 运行 | 实际结果 |
| --- | --- |
| MSVC Debug / CTest | 3/3 通过；模型默认 200,000 次随机事件，callback 默认 200 轮，task 默认 100 轮 |
| ASan / 模型定向场景 | 20 个通过；5 种故意缺陷全部被 oracle 拒绝 |
| ASan / 模型有界搜索 | 深度 10，最多 3 个 records；30,223 个状态，63,792 条转换 |
| ASan / 模型随机事件 | 50 个固定种子 × 100,000 次事件；共 5,000,000 次，覆盖 16 类事件，最多 8 个 records |
| ASan / callback | 12 类用例；其中 7 类并发用例各重复 10,000 轮，通过 |
| ASan / task 生命周期 | 8 类用例各重复 5,000 轮，共 40,000 个用例，通过 |

这些结果验证了无 branch 哨兵时不产生责任，以及已持有哨兵的 stopped task 在
结果被转为 value 后保留 frame 的条件。测试没有启动真实 Pause 屏障，因此
没有把“真实库的 frame 保留 + 模型中的暂停责任”报告成完整 Pause 死锁复现。

## 本轮扩展后的运行记录（2026-09-22）

完整发现、计数口径、修改范围和未覆盖验收项见
[设计审查与测试报告](design-review-2026-09-22.md)。该轮实验未修改 `pause.hpp` 或设计文档；
后续设计采用决定见本文开头，以下保留当轮结果。

| 运行 | 实际结果 |
| --- | --- |
| MSVC Debug / CTest | 4/4 通过；最后补充满二叉场景后，受影响 task 测试再次 1/1 通过 |
| ASan 模型定向/故障注入 | 193 个定向通过，其中新增 173 个完整嵌套场景；7/7 故意缺陷被拒绝 |
| ASan 模型有界搜索 | 事件深度 10，3 records；30,223 个状态，63,792 条转换 |
| ASan 模型随机事件 | **100 个种子 × 100,000 = 10,000,000**；9,788,868 成功事件 + 211,132 预期 API 拒绝 |
| 随机嵌套覆盖 | 499,650 次 fork，其中 391,141 次嵌套；最深 **21 层**，宽度最高 3，峰值 24 records |
| ASan 原 callback | 5 个单次用例 + 7 类 × 10,000 = 70,005 个用例通过 |
| ASan 确定性 callback 钩子 | 7 类 × 10,000 = 70,000 个用例通过 |
| ASan task 生命周期 | 最终 25 类 × 5,000 = **125,000** 个用例通过，包含 3/5 层链状展开、3 层满二叉的四类结果路径 |

四组 ASan 均未报告内存错误；未运行 TSan。随机模型种子基值为
`344693494597`，参数为 `--depth 10 --random-steps 100000 --seeds 100 --max-records 24`。
1000 万事件不是 1000 万完整场景，也不是真实 Pause 集成验收；默认 `exec::task`
恢复 scheduler 的 token 擦除仍是实现者必须解决的集成约束。

## 组件取消对照（2026-09-23）

在相同固定依赖与 MSVC 工具链下，新增独立的
[临时探针](../build/standards-review-20260923/scheduler_stop_probe.cpp)，
使用公开组件 API，100 轮确定性对照全部通过，进程设 30 秒总超时。
前三条路径使用同一个 source 的同一个 `inplace_stop_token`，均先 start 入队、
确认未完成，然后 request_stop，最后执行 run_loop 队列。

| 路径 | 实测结果 |
| --- | --- |
| 具体 `run_loop` scheduler | stopped |
| `stdexec::task_scheduler(run_loop_scheduler)` | stopped |
| 旧 `exec::task` 暴露的 current scheduler | value，完成时原 token 已停止 |
| `inline_scheduler`，start 前 token 已停止 | value，属于不检查取消 |
| `stdexec::task` 的 current scheduler 显式调度，入队后 stop | 根操作 stopped，后续语句未执行 |

编译/运行命令（仓库根目录；构建脚本使用本机 Visual Studio 路径）：

```powershell
.\build\standards-review-20260923\build-scheduler-stop-probe.cmd
.\build\standards-review-20260923\run-scheduler-stop-probe.cmd
```

[原始输出](../build/standards-review-20260923/scheduler_stop_probe.out)
与 stderr 已保留。这是组件行为验证，没有切换主设计使用的 task、没有实现 Pause，
也没有复跑 1000 万次模型事件；这里描述的是该探针执行时的状态，后续设计已采用
本文开头的新方案。本探针未启用 ASan；前一日的 ASan 结果仍仅适用于
当日列出的测试。模型将调度结果作为外部事件，不能替代真实恢复取消的集成验收。

## 新 task 的项目可行性验证（2026-09-23）

新旧 task 的配置、环境、取消、全通道 affinity 和生命周期差异，以及新增测试的
覆盖、结果和复现命令，见[可行性报告](stdexec-task-feasibility-2026-09-23.md)。
新增三组测试使用现有 `PAUSE_BUILD_DESIGN_TESTS` 开关；旧测试继续作为对照，
可行性实验阶段未切换生产 task、修改设计契约或升级依赖。实验完成后，用户已批准
将主设计迁移到本文开头的方案；`pause.hpp` 仍待实现。

全量 Debug CTest 7/7 通过，最终 API 静态约束补充后定向1/1通过；新增 ASan
用例合计213,000（API 65,000、生命周期115,000、调度33,000），均无ASan报告。

新 task 的 token 传播可以满足项目要求，但直接替换会增加宿主调度。
公开 `.affine()` 定制现已选为全通道宿主适配的设计方向；其依据仍是独立探针，
真实 Pause 的内部终止策略、
责任撤销和完整 fork 生命周期仍需在实现后验收。
# 真实 Pause 实现验收（2026-09-23）

本轮结果、MSVC Debug STL 内部终止的资源保证边界，以及 Linux TSan 的检测边界，
见[实现与验收报告](pause-implementation-validation-2026-09-23.md)。

`pause.hpp` 已实现设计中的公共 API、暂停记账、callback 完成权、恢复调度及
默认 `stdexec::task` 的 `.affine()` 适配。下方历史记录中的“实现为空”描述保留为
当时的实验背景。真实实现测试与设计模型／依赖探针分开统计。

真实验收使用 `PAUSE_BUILD_TESTS`（默认 ON）；历史探针继续使用
`PAUSE_BUILD_DESIGN_TESTS`。`pause` 是 header-only 的 CMake `INTERFACE` 目标，
不生成静态库或动态库；以下 Debug/Release 仅指包含该头文件的测试程序。
两套预设继承公共测试配置，保留各自默认的优化、断言、CRT 和 STL 迭代器级别。
从仓库根目录执行全部两种配置的测试：

```powershell
.\scripts\msvc-cmake.cmd test
```

也可以分别执行；每次包含配置、构建及全套 CTest：

```powershell
.\scripts\msvc-cmake.cmd test debug
.\scripts\msvc-cmake.cmd test release
```

`configure`、`build` 同样接受 `debug`、`release`、`all`，不指定时仍默认
Debug；`test` 不指定时默认 `all`。在 Visual Studio Developer PowerShell
中也可直接使用同名 `msvc-debug`／`msvc-release` 的 configure、build 和
test presets。Release 用例的 `CHECK` 不依赖 `assert`，不会被 `NDEBUG` 移除；
正负编译用例继承所选配置的编译参数及 CRT，日志按配置隔离。

真实测试覆盖公共 API、初始模式、epoch、空屏障、原子 fork、parent 重入、
资源回滚、真实 task 生命周期、独立取消范围、多执行资源、完成时销毁、
callback 注册／选中／注销窗口和内部错误终止。负编译测试先验证有效环境
能编译，再确认缺 scheduler／错误 token 被拒绝；头文件测试包含两个翻译单元。

增加真实线程竞争次数并指定可复现的随机种子：

```powershell
.\build\msvc-debug\pause_integration_test.exe --iterations 10000 --seed 0x3f09b7a561d82c45
.\build\msvc-debug\pause_sender_test.exe --iterations 10000
.\build\msvc-debug\pause_sender_hook_test.exe --iterations 10000
```

将路径中的 `msvc-debug` 换为 `msvc-release`，即可验证相同用例的 Release
构建；两种配置的运行证据分别记录，不能用一种配置的结果替代另一种。

完整 ASan 验收在 x64 Visual Studio Developer PowerShell 中执行：

```powershell
.\scripts\run-pause-stress.ps1 -Iterations 10000 -ModelSteps 100000 -ModelSeeds 100
```

脚本重新编译全部真实测试和历史探针，逐进程设超时并保留构建日志、stdout、
stderr、退出码和 SHA256。主模型是 1000 万个事件，不是 1000 万个工作流；
实际线程场景数量由各 executable 单独输出。ASan 不能替代 TSan。

本机 WSL 的 TSan 复现命令如下；脚本使用测试专用 POSIX 声明 shim 和单进程
`setarch -R`。这不是对原生 Linux 公共 API 兼容性的承诺，详见验收报告。

```sh
bash scripts/run-pause-tsan.sh --iterations 1000
```

测试钩子只插入构建目录中的固定 stdexec 副本，先核对原头 SHA256；生产
依赖和 `pause.hpp` 没有测试钩子。用于截停通知尾部的阻塞通知器仅为受控
故障注入；生产通知器仍须遵守文档的非阻塞、线程安全和不同步重入要求。

## clice 智能提示

`.clice/config.toml` 按[官方配置文档](https://docs.clice.io/clice/guide/configuration)
声明 `debug`、`release` 两个配置，分别读取
`build/msvc-debug/compile_commands.json` 和
`build/msvc-release/compile_commands.json`，默认使用 `debug`。
上述测试命令会生成两份编译数据库；只需生成时可运行
`.\scripts\msvc-cmake.cmd configure all`。

clice 从编译数据库取得 C++ 标准、宏和依赖路径，并自动查询 MSVC 工具链。
无需复制 `.clangd` 的配置，也无需手工维护系统头文件路径。
本机 clice `g900128f` 在生成该项目的 PCH 时发生内置 Clang 断言崩溃，
因此配置采用官方支持的 `[project] readonly = "on"`：查询主要由索引提供，
补全／签名提示按需编译且不使用 PCH。它不限制编辑源码或构建；代价是没有
PCH 复用，本次真实补全请求约 2.6–2.7 秒。后台索引保留默认开启。
Debug、Release 的真实成员补全均已验证；升级 clice 后可重新验证 PCH，
确认修复后移除此兼容设置，恢复默认的完整 AST 服务方式。

配置文件仅在启动时读取：已打开 VS Code 时执行
`Clice: Restart Language Server`；使用 `Clice: Switch Build Configuration`
可切换 Debug/Release 智能提示上下文。这个选择不改变库或测试构建配置。
