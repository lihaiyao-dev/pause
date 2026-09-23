# stdexec::task 与 exec::task：项目可行性验证

基线：NVIDIA stdexec `f0e8ae6fdc6c188389b146bb854b80d399724b04`，
Windows x64、MSVC 19.44.35225。本报告记录的可行性实验保持依赖、生产 API 和
[Pause 设计](../pause-branch-design.md)不变；这是实验执行时的状态。
实验完成后，用户已批准下述设计迁移，当前设计已采用新方案；`pause.hpp` 仍为空。
报告中的测试结果验证宿主组件、协议探针和所有权模型，不是实际 Pause 集成验收。

## 当前采用决策与依据

已选择 **`stdexec::task` + 全通道 affine 宿主适配 + 审核过的 scheduler**，
首版限定已审查的 **`run_loop` + 默认 allocator** 路径，固定依赖版本不变。
这是一项设计采用决定，尚不表示生产实现完成或完整验收通过。

不能直接改命名空间后认为全部契约成立。新 task 保留了本项目必需的恢复调度
stop token；直接套用实验时既有的 sender 属性时，
会额外添加 affinity 调度，与 Open 零调度、正常恢复一次调度的目标冲突。

实验阶段的方案比较保留为决策依据：

1. **已采用的设计方向**：使用新 task，并为直接等待 Pause 的路径提供真正满足全通道 affinity 的宿主适配。
   保留原恢复调度的取消通道，仅对需要返回宿主执行资源的终止路径补调度。
   独立探针验证此公开扩展方向，生产 Pause 的适配仍待实现。
2. 未采用：保留旧 task，另行修复或配置 scheduler 类型擦除的取消查询。
   既有生命周期与 value affinity 方案可继续使用，但旧默认配置仍不满足恢复取消。
3. 未采用：直接替换成新 task 并接受额外调度，改动较小，却会改变现有调度契约。
   没有因此放宽验收要求。

已选方案须按当前设计区分宿主 stopped 调度的计数边界，并在实现中保证队列排空
与 frame 生存期。下文的历史实验计数不等于生产适配已经实现。
不能将“value 路径恢复 0/1 次”概括为“现有设计全部无需调整”。

## 两套 task 的实质差异

两者都来自当前固定依赖，不是一个别名的两个拼写。`stdexec::task` 跟随标准
task 方向；这并不表示当前实现与最新草案完全一致，也不是本机标准库的
`std::execution::task`。

| 方面 | `exec::task` | `stdexec::task` | 项目影响 |
| --- | --- | --- | --- |
| 头文件与类型 | `<exec/task.hpp>`；默认 `basic_task<T, default_task_context<T>>` 别名 | `<stdexec/execution.hpp>`；`task<T = void, TaskEnv = env<>>` | 无须升级依赖，但不是接口等价替换 |
| 配置入口 | task context | `TaskEnv` 的 scheduler、stop source、allocator、error types、环境类型 | 不能原样复用 context 定制 |
| 默认恢复 scheduler | 旧 `any_scheduler` 擦除 stop 查询 | `task_scheduler` 保留 `inplace_stop_token` | 新 task 更适合已确定的恢复取消要求 |
| 环境查询 | 默认 scheduler 与 stop token | 默认 scheduler、allocator、stop token；其他查询需要配置 `TaskEnv` | 不是任意外层环境自动全量转发 |
| affinity 检查 | value 通道 | value/error/stopped 三通道 | 现有仅 value 属性不足以避免额外调度 |
| frame 释放 | value/error 的 await_resume 释放；stopped 可保留至 operation 析构 | 总体边界相同 | optional 包装与结构化所有权规则仍需保留 |
| 默认错误传播 | `exception_ptr` | `exception_ptr` | 不改变 Pause 内部错误必须终止的要求 |
| 自定义错误 | 旧 context 体系 | 提供 `error_types` / `with_error`，但此版本 sender 输出有偏差 | 本项目未使用该扩展，不作为迁移前提 |

### 环境与取消

- 外层已经提供 `inplace_stop_token` 时，新 task 保存同一个 token。
  `when_all` 提供组内 token 是合法边界，不能要求它等于根 source 的 token。
- 外层 token 类型不同时，默认新 task 通过 callback 桥接为自己的 inplace token。
  API 探针覆盖了已停止的 `std::stop_token`。
- token 已请求停止不意味着裸 task 的所有语句都禁止执行：取消仍是协作行为。
  生命周期预取消用例通过具体 `starts_on` 调度接受取消，验证 body 未进入；
  它不等价于任意裸 task 都有启动前自动取消检查。
- 自定义 forwarding query 在默认新 task 内不可见，配置 `TaskEnv` 后可见。
  缺少 scheduler 时默认 task 的 `sender_in` 检查拒绝该环境。
- Pause 自己的公开 receiver 仍须检查实际 token 类型和 scheduler 查询；task 的
  转换能力不能替代设计第 3.4 节的环境约束与运行期验证。

### 调度与 affinity

新 task 识别当前 `exec::get_completion_behavior_t`，问题不在属性名字。
它通常调用 `stdexec::affine`；该算法需要所有可能的完成通道都保持启动资源。
实验时既有的 Pause sender 属性只保证 value，stopped 可以来自 callback 线程，
因此未经所选宿主适配时，默认回退采用：

```cpp
// 示意新 task 的 affinity 回退；这是额外宿主调度。
finally(original_sender, unstoppable(schedule(current_scheduler)))
```

不能将 stopped 虚报为 affine。公开 `sender.affine()` 可定制实际适配：

```cpp
// 已选宿主适配方向的独立探针示意，不是生产 Pause 实现。
affine_pause_sender = handle_completion(pause_sender,
    value   => deliver_value_directly(), // 保留 Open 0 / Resumed 1
    stopped => schedule_host_uncancellable_then_deliver_stopped());

// pause_sender 原有恢复 schedule 仍携带原 token，仍接受取消。
// Pause 内部 error 在进入此宿主层前必须 terminate。
```

只有额外的宿主 affinity 跳转不可取消；原暂停恢复调度不能使用 `unstoppable`。
宿主可能因 stopped 多调度一次，因此必须保活 scheduler、队列和 operation 到最终
完成。探针不把这一调度记作原始 Pause 恢复调度；实验期间并未修改设计中的计数定义。

外包 `then` 等 adaptor 后，具体 sender 的 `.affine()` 成员可能被隐藏，宿主会再次
采用默认适配。原设计的无额外调度保证本就限定直接 co_await，已选方案仍保留此边界。

### scheduler 与错误兼容性

默认 `task_scheduler` 不接受任意满足 scheduler concept 的类型：它要求不可取消环境下
调度没有 error/stopped，并限制擦除 operation 的大小/对齐。API 测试静态确认：
一个合法但带 error 签名的 scheduler 不能构造默认 `task_scheduler`。
可配置 `TaskEnv::start_scheduler_type` 使用具体 scheduler，但普通 affine 回退仍有
无失败调度的要求，不能据此声称任意 scheduler 都能接入。

还发现一个固定实现的异常处理边界：

- `__task_scheduler.hpp:744–795` 的调度后端调用 `connect`，并在异常分支使用
  `std::unreachable()`；两条普通 schedule 路径均选择该分支。这是未定义行为，
  不能当作项目要求的 `std::terminate()`。
- “没有 error 完成签名”不保证 `connect` 不抛异常。API 测试用一个合法、
  无 error 签名但 `connect` 可抛 `bad_alloc` 的 scheduler，静态确认构造约束接受它。
  没有执行通向 `unreachable` 的路径，不以某次崩溃结果证明其安全。
- 擦除存储大小检查只覆盖不可取消环境；自定义 scheduler 若在可取消环境需要更大
  operation，后端还可能动态分配，并触及相同异常分支。
- 对本轮具体 `run_loop` + 默认 allocator 的源码路径，`schedule/connect` 均
  `noexcept`，operation 大小不随 token 环境变化，可放入预留存储，无该额外分配。
  因而未将已测 run_loop 路径判为失败，但不能把结果推广到任意合法 scheduler。

当前采用决定据此将首版限定为已审查的 `run_loop` + 默认 allocator 路径。
后续扩展 scheduler 或 allocator 前必须重新验证支持范围，必要时修复相应适配/上游失败路径。
仅补一条“要求所有内部错误 terminate”的文档约定不足以修复上述库内路径。

已验证移动专属返回值、引用返回值、awaited error 可捕获、stopped 不经过 catch。
显式协程 frame allocator 的正常/value/error/stopped/未启动销毁都无泄漏；
frame 分配拒绝以构造阶段 `bad_alloc` 抛出，body 未进入。这不等价于已经验证
task_scheduler 后端分配失败或 Pause 启动后的内部错误终止策略。

固定版本的类型化错误存在实现与草案差异：`co_yield with_error{23}` 经普通 sender
连接后收到 `exception_ptr`，重新抛出才取得 `int(23)`。测试明确保留此事实对照，
不把它当作类型化错误契约已满足。本项目默认 `exception_ptr` 路径不受此差异影响。

## 测试范围

| 测试文件 | 覆盖 |
| --- | --- |
| [API/resource](stdexec_standard_task_api_test.cpp) | 13 类/轮；返回值、error/stopped、环境、token 桥接、frame 分配与释放；另有编译期环境与 scheduler 约束 |
| [lifetime](stdexec_standard_task_lifetime_test.cpp) | 23 类/轮；直接/包装三通道、3/5 层链状和3层二叉分支的四类退出路径、父子退役次序、兄弟取消、预取消、跨线程 start/completion |
| [scheduler](stdexec_standard_task_scheduler_test.cpp) | 初轮33类、采用后35类/轮；实际 schedule/start 计数、直接/外包 adaptor、when_all token 边界、排队后取消、两个执行资源、异线程 stopped/error、公开 affinity 定制及撤责先于宿主跳转 |

生命周期中的树节点是测试所有权哨兵。它检查孩子先于父亲释放以及 stopped frame
的保留条件，不调用尚未实现的 PauseBranch::fork。单次 completion 与 start 尾部使用
latch 控制，未用 sleep 作为正确性依据；stopped 握手的探针只定位尝试交付，不能证明
执行已进入库内特定自旋指令。
二叉分支的正常路径核对完整15节点；error/stopped 可以在子树全部展开前传播，
这些路径验证实际构造的树，未声称每次都展开完整15节点。

## 可行性实验原始运行结果（采用前）

| 运行 | 结果 |
| --- | --- |
| MSVC Debug 全量 CTest | 7/7 通过，包含原4组回归及新增3组，25.37秒 |
| 最后补充 API 编译期约束后的定向 CTest | 1/1 通过 |
| API/resource ASan | 13类 × 5,000轮 = **65,000**；通过，stderr为空 |
| lifetime 独立 Debug | 23类 × 5,000轮 = **115,000**；通过，43.05秒 |
| lifetime ASan | 23类 × 5,000轮 = **115,000**；通过，130.46秒，stderr为空 |
| scheduler 独立 Debug | 33类 × 1,000轮 = **33,000**；通过 |
| scheduler ASan | 33类 × 1,000轮 = **33,000**；通过，无ASan报告 |

本轮新增 ASan 合计 **213,000** 个用例；不重复累计短跑或重跑。
这些“通过”包括明确的库行为负对照，例如默认属性多调度、直接 stopped 转换保留
frame、自定义 typed error 被擦除。它们表示探针正确检出事实，不表示这些事实满足
全部项目契约。

调度实测先单独核对 `starts_on(empty_task)` 的宿主基线：底层 schedule 工厂调用
**2次**、operation start **1次**。以下是扣除对应基线后的增量；两类增量相同：

| 等待方式 | Open | 正常 Resumed |
| --- | ---: | ---: |
| 实验时既有的仅 value 属性 | 1 | 2 |
| 公开 `.affine()` 适配探针（现已选用方向），直接 co_await | 0 | 1 |
| 同一适配探针外包 `then` 后 co_await | 1 | 2 |

适配探针的 stopped/error 增加一次真正的宿主 affinity 跳转。异线程用例确认原始完成线程
不同于最终宿主资源；恢复排队后的取消仍由原恢复 schedule 观察并交付 stopped，
额外不可取消跳转未将 stopped 改为 value。

原始记录位于 `build/standard-task-review-20260923/`：

- `ctest-7.log`、`ctest-api-final.log`。
- `api-asan.stdout.log` / `api-asan.stderr.log`。
- `debug-5000.*` / `asan-5000.*` 与 `lifetime-results.md`。
- `scheduler-debug-run.log` / `scheduler-asan-run.log`。

独立运行设置进程总超时；lifetime长跑上限300秒，API上限30秒。
最初在未初始化MSVC环境中启动ASan时出现缺少运行时DLL的加载失败，未进入测试；
上表是通过正确工具链环境启动后的最终结果。

可行性实验阶段的改动文件为本报告、`tests/README.md`、上述三个新测试和根 `CMakeLists.txt`。
该阶段未修改旧测试、主设计、`pause.hpp` 或依赖版本，没有公共 API 或数据迁移。
这是历史实验范围；随后主设计已按本文开头的采用决定迁移，生产 `pause.hpp` 仍未实现。

## 采用方案后的修改与验证

主设计现已明确默认 `stdexec::task`、本体之外的 `.affine()` 宿主适配及
run_loop/default allocator 接入范围，给出正常/取消路径的分层调度计数。
stopped 必须先由本体撤销责任并通知，再进入宿主跳转；新增适配层的内部 error
另有最外层终止包装，不在错误处理中重复记账。

宿主使用骨架已改为 run_loop，完成全部任务和控制调用后才 finish/join，
使用固定依赖的 `exec::scope_guard` 覆盖启动失败。工具链 smoke 实际执行新 task、
run_loop 和此 guard，不再只包含旧 task 头文件。

调度探针保留原33类，新增2类：

- 外线程真实 stop callback 完成后，责任哨兵已撤销，宿主跳转已排队但 task 尚未完成。
- Resumed 入队后请求 stop；原调度接受 stopped 才撤销责任，随后宿主跳转才完成 task。

两例均使用 latch 暂停队列并检查停止只交付一次、不执行后续业务；责任哨兵为
测试协议状态，不是真实 Pause 的 pending/live。生产 callback/SingleStep 仍须验收。

本次实际结果：Debug CTest **7/7** 通过（27.47秒）；最终新工具链 smoke 构建并运行
通过，退出码0；调度 ASan **35类 × 1,000轮 = 35,000** 通过，退出码0、stderr为空。
没有重跑未变的生命周期/API长跑或1000万模型压力，也没有把这些历史次数累计为
本轮新增覆盖；未运行 TSan。

日志及本机独立复现脚本在 `build/task-adoption-20260923/`：`ctest-7.log`、
`smoke.stdout.log` / `smoke.stderr.log`、`scheduler-asan.stdout.log` /
`scheduler-asan.stderr.log`、`build-scheduler-asan.cmd`、`run-scheduler-asan.cmd`。
本轮修改主设计、两份测试说明、调度探针及工具链 smoke；生产 `pause.hpp`、
公共 API 和固定依赖未变。

## 复现

三组测试已纳入现有 `PAUSE_BUILD_DESIGN_TESTS`，无新依赖：

```powershell
.\scripts\msvc-cmake.cmd test
```

在 Visual Studio x64 Developer PowerShell 中独立构建 ASan（`/RTC1` 不可并用）：

```powershell
New-Item -ItemType Directory -Path build/standard-task-asan -Force | Out-Null
$testNames = @('stdexec_standard_task_api_test',
               'stdexec_standard_task_lifetime_test',
               'stdexec_standard_task_scheduler_test')
foreach ($testName in $testNames) {
    & cl.exe /nologo /std:c++latest /Zc:__cplusplus /Zc:preprocessor `
        /EHsc /MD /O2 /Zi /fsanitize=address `
        /Ibuild/msvc-debug/_deps/stdexec-src/include `
        "/Febuild/standard-task-asan/$testName.exe" `
        "/Fobuild/standard-task-asan/$testName.obj" `
        "/Fdbuild/standard-task-asan/$testName.pdb" "tests/$testName.cpp"
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $testName" }
    & "./build/standard-task-asan/$testName.exe" --iterations 5000
    if ($LASTEXITCODE -ne 0) { throw "Test failed: $testName" }
}
```

## 源码依据与剩余验收

以下路径相对于固定依赖的 `include/`：

- `exec/task.hpp:49–53,601–615,660–698`：旧 scheduler 擦除、value affinity、frame 释放。
- `exec/completion_behavior.hpp:63`：公开属性 query 与新实现使用同一类型。
- `stdexec/__detail/__task.hpp:278–300,442–450,513–538,637–647,726–759`：
  新 task 配置、token、释放、await transform、环境。
- `stdexec/__detail/__affine.hpp:50–53,92–95,124–125`：全通道判断、成员定制、默认回退。
- `stdexec/__detail/__task_scheduler.hpp:241–258,784–794`：兼容约束与 token 转发。
- 同文件 `655–690,744–795`：operation 存储、connect 与异常分支；
  `stdexec/__detail/__run_loop.hpp:121–156,263,301`：本轮具体后端的布局和 noexcept。
- `stdexec/__detail/__connect_awaitable.hpp:470–488` 及
  `__get_completion_signatures.hpp:110–115`：类型化错误经通用连接转为 exception_ptr。

标准对照使用此前取得的 cplusplus/draft 提交
`e908596f0b9031e53f334f375f86367808bb99e9`（2026-09-21），
参见 [affine](https://eel.is/c++draft/exec.affine) 和
[task](https://eel.is/c++draft/exec.task)。在线链接会随草案更新，固定快照在
`build/standards-review-20260923/exec.tex`。

本轮没有运行 TSan、其他编译器或性能 benchmark。ASan 与重复交错不能证明没有数据
竞争；调度计数也不能替代吞吐/时延测量。此前1000万模型事件无需因更换探针再跑，
但也不能当作新 task 真实 Pause 集成的1000万次验收。

设计方向已经选定；生产 Pause 实现后仍需 callback 注销、责任撤销、SingleStep、同步重入、
operation 销毁、真实嵌套 fork、调度构造/connect 失败与终止策略验收。
原可行性实验提出需更新设计第 2.3/3.4/6.5/9/10/14.3，当时未执行迁移；
当前宿主说明与测试目标以已迁移的[主设计](../pause-branch-design.md)为准。
