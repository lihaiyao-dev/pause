# Pause 实现与验收记录（2026-09-23）

## 实现与当前交付边界

`pause.hpp` 已按 `pause-branch-design.md` 实现。固定 stdexec 提交保持
`f0e8ae6fdc6c188389b146bb854b80d399724b04`，未引入第三方依赖，也未改变
公共命名空间、返回容器或取消通道。

Pause 是 header-only 库，CMake 的 `pause`／`pause::pause` 仅提供 INTERFACE
使用要求，不生成静态库或动态库。本文的 Debug、Release、CRT 和 STL 迭代器
设置均描述包含该头文件的宿主／测试翻译单元，不是不同版本的库制品。

**保留 MSVC 默认 Debug 配置，接受 STL 的 noexcept 内部失败终止。**
资源失败注入发现：本机 MSVC 14.44 STL 在 `_ITERATOR_DEBUG_LEVEL=2` 时，
`std::vector` 默认构造和 `noexcept` 移动构造仍会分配 iterator proxy。
分配失败在 STL 的 `noexcept` 内直接终止，Pause 外层不能捕获。
用户已明确接受这类终止；第 11.3 节据此明确资源保证边界。动态 fork 的
vector 元素存储和 branch record 分配失败仍必须返回 `ResourceExhausted`，
保持完整原状态、无额外通知和无泄漏；STL 的 noexcept 内部失败不承诺回滚。
返回结果时的 proxy 分配还可能发生在共享状态已提交之后。

此前提出的 `_ITERATOR_DEBUG_LEVEL=0` 仅用于消除该调试代理分配，现不采用，
未写入生产 CMake interface 或 preset。此设置会关闭 STL 迭代器调试并改变
相关 STL ABI，而不关闭 `assert`、Debug 信息、Pause 不变量检查或 ASan。
这里的 `noexcept` 只说明异常不能向调用方传播，不能据此认定零分配或
认定 MSVC 违反标准；本项目的终止策略与标准符合性判断分开处理。
微软已在 [STL issue #1035](https://github.com/microsoft/STL/issues/1035) 将
Debug 模式低内存导致此类终止列为已知缺陷；维护者也确认了问题和修复方向。

## 核心实现

- Source 按值拥有唯一状态锁、branch 和 Waiting 两条非拥有侵入链表。
- 创建和 fork 先准备完整资源，再在锁内一次提交；array/vector 共用 fork 提交。
- 到达、取消、公开最终 stopped、fork 转交和 RAII 退役共用责任撤销及提交逻辑。
- 重入与检查暂停门在同一事务完成；SingleStep 在旧 waiter 交付前建立下一轮。
- callback 构造期只记录取消；状态锁决定唯一完成方和唯一显式注销方。
- Resume 在锁外先注销整批 callback，再逐项摘出并交付，terminal CPO 后不访问节点。
- 公开 sender 惰性读取原 scheduler；Open 同步 value，等待后恢复才提交恢复调度。
- 记账包装和宿主包装共用完成策略模板；内部 error 直接 terminate，签名保守保留。
- `.affine()` 只为已确定的 stopped 添加不可取消宿主跳转，先撤责再跳转。
- 系统加锁异常经共用 `noexcept lock_state` fail-fast，不泄露为控制 API 的恢复错误。
- Debug 每次复合提交重建 live/pending/waiter/直接 child 关系，检查设计不变量。

## 真实实现测试结果

使用原 `msvc-debug` preset（默认 `_ITERATOR_DEBUG_LEVEL=2`）重新构建并执行
全量 **CTest 19/19 通过**，测试耗时 41.51 秒，日志在
`build/msvc-default-final.log` 和 `build/msvc-debug/Testing/Temporary/LastTest.log`。
`/EHsc` 已统一放到库的 interface 编译选项，使多 TU 和下游使用者具备
资源回滚需要的异常展开语义。

资源测试先校准本机 vector 默认/移动构造各一次代理分配，再在两种模式下
对动态四子分支 fork 的三个 proxy 分配点分别运行独立子进程，共 **6 个**。
仅实际注入 bad_alloc、分配序号和提交通知阶段一致且命中 terminate handler
的退出码 77 才通过；普通返回、其他异常和任意进程崩溃都失败。其余元素
存储及四个 branch record 分配点逐一验证回滚与无泄漏；未来 STL 的分配
布局改变会明确报错，需要重新核对这些测试点，不会静默缩减覆盖。

此前隔离的 `build/msvc-acceptance-debug/` 在显式设置迭代器级别 0 时也有
19/19 通过记录；它仅保留为排查证据，未采用该候选设置。

以下 ASan 测试使用 MSVC 19.44 x64，`/std:c++latest /EHsc /MD /O2 /Zi
/fsanitize=address`，没有定义 `NDEBUG`，Pause 断言仍生效。所有列出的进程
退出码均为 0，stderr 为空。

| 最新源码测试 | ASan 实际结果 |
| --- | --- |
| `pause_integration_test` | 20 类 × 10,000 轮 + 4 个深链，共 200,004 场景 |
| `pause_sender_test` | 27 类 × 10,000，共 270,000 场景 |
| `pause_sender_hook_test` | 7 类 × 10,000，共 70,000 确定性交错 |
| `pause_sender_death_test` | 6 个独立进程均命中 terminate handler；外部 let_error 未执行 |
| `pause_resource_test` | 最终 19 类 × 10,000，共 190,000 场景 |
| `pause_task_lifetime_test` | 3 类 × 10,000，共 30,000 场景 |
| 合计 | **760,010 个真实实现场景／子进程** |

集成压力每轮建立 4 个 run_loop、4 个控制器、4 个 stopper 和 observer 的竞争。
10,000 轮共 40,000 次竞争分支完成，value=773、stopped=39,227，均恰好一次。
另外各有 10,000 轮 fork/request_pause 和 parent 重入/resume 随机竞争，固定种子
`4542363620118899781`（`0x3f09b7a561d82c45`）。PRNG 决定模式、数量、取消子集
及 yield 扰动，barrier/latch 建立交错，不用 sleep 判断正确性。

真实 Pause 的 32/64 层链分别通过 Continuous 和 SingleStep，检查 65/129 个
暂停点、逐层 fork/join/reentry 和责任排空。这是有界验收，不是任意深度安全保证。

主 ASan 日志在 `build/pause-acceptance-asan/`。资源文件增加默认 task 的四条
无分配路径后，最终 19 类重新编译、运行；最终资源及补充生命周期日志在
`build/pause-acceptance-extra-asan/`。主目录的旧资源二进制仅有 15 类，其
150,000 场景不计入上述最终源码总数，也不作为最终资源源码的二进制凭证。
接受默认 Debug 的 STL 内部终止边界后，更新的资源测试再次重新编译并执行
19 类 × 10,000 轮；最新 190,000 场景日志在 `build/pause-resource-final-asan/`。
上表资源结果使用这次最新批次替换此前批次，不重复累计。ASan 使用 /MD，
其迭代器级别为默认 0；新增六个 proxy 终止子进程由上述默认 Debug 验证。

## 模型与既有组件回归

本轮重新编译的最终模型源码（SHA256
`DF9B4699D6DDF411DC68BD4C5690386E9CC3A182866AFC74FE1F407CAADB655A`）
在 ASan 下执行了 227 个定向场景、7 类故意缺陷拒绝、有界搜索以及完整
**10,000,000 个随机事件**，退出码 0，耗时 1358.67 秒，stderr 为空。
没有将历史基线的运行结果冒充本轮最终源码结果。

| 模型指标 | 本轮实际值 |
| --- | ---: |
| 有界搜索事件深度／最多 records | 10 / 3 |
| 有界状态／转换 | 30,223 / 63,792 |
| seed 基值／数量／每 seed 事件 | 344693494597 / 100 / 100,000 |
| 成功事件／预期 API 拒绝 | 9,788,868 / 211,132 |
| fork／其中嵌套 fork | 499,650 / 391,141 |
| 最深 fork／最大宽度／峰值 records | 21 / 3 / 24 |
| 构造期取消／Waiting 取消／scheduler stopped | 807,460 / 154,883 / 29,661 |
| 完整退役 scope | 226 |

模型探索逻辑事件并重建不变量；上述 1000 万事件不是完整工作流，也不能代替
真实 C++ callback／侵入节点的生命周期和内存序检测。真实实现结果另列于上文。

其余完整 ASan 回归也已执行完毕，均退出 0、无超时、stderr 为空：

| 组件探针 | 实际场景数 |
| --- | ---: |
| 真实 stop callback | 70,005 |
| 确定性 stop callback 钩子 | 70,000 |
| callback／let_stopped／affinity 生命周期 | 40,000 |
| 旧 exec::task 生命周期对照 | 250,000 |
| stdexec::task API／资源 | 130,000 |
| stdexec::task 生命周期 | 230,000 |
| stdexec::task scheduler | 350,000 |
| 组件压力合计 | **1,140,005** |

另有 72 个组件深链场景（默认 48 个 + 32/64 层 × 两种拓扑 × 六种结果），
以及 64 层完整模型的 12 个 scope、21,128 个逐事件检查，全部通过。
主 ASan 脚本的 39 次独立运行均成功；以最终资源重跑替换旧资源批次、再加入
真实 task 生命周期补测后，最终清单含 40 次成功运行。

机器可读的汇总在 `build/pause-final-validation.json`，包含最终实现 SHA256、
各二进制与源码哈希、参数、退出码、耗时及编译配置。旧资源批次记录的
源码哈希已标为不可用于二进制对应证明，最终 19 类批次保留编译时哈希。

## ThreadSanitizer

WSL Debian 13、GCC 14.2、`-std=c++23 -pthread -g -O1 -fsanitize=thread
-fno-omit-frame-pointer -fno-pie -no-pie`：

| 测试 | TSan 结果 |
| --- | --- |
| sender | 27,000 |
| integration | 20,004，包含 4 个真实深链 |
| callback hook | 7,000 |
| task lifetime | 3,000 |
| 合计 | **57,004 场景通过，无 TSan 数据竞争报告** |

Windows 原生 Clang 不支持本机 target 的 TSan。WSL 初始运行报告 unexpected
memory mapping；仅对测试进程使用 `setarch -R` 后完成上述测试，未改变系统配置。

POSIX 的全局函数 `::pause()` 与设计要求的 `namespace pause` 冲突，Linux 标准库
间接包含 `unistd.h` 时会编译失败。TSan 测试用预包含 shim 只重命名未调用的
POSIX 声明，然后测试实际 Pause 代码；生产头和公共 API 均未改名。
**这提供内存竞争检测证据，不代表未经 shim 的原生 Linux 接入兼容。**

日志为 `build/tsan-tests/sender-1000-run.log`、`integration-1000-run.log`、
`pause_sender_hook_test-run.log`、`pause_task_lifetime_test-run.log`，对应 exit 文件均为 0。

## 验收映射及结论范围

| 设计章节 | 实际检查 |
| --- | --- |
| 14.1 API／收敛 | 静态类型约束、错误映射、两种初始模式、空屏障、epoch、同轮重入、逐事务不变量 |
| 14.2 callback／释放 | 注册可见但构造未返回、选中后延迟进入、Resume-first／callback-first／Open-first、异线程注销等待、同步自毁、异线程销毁、start 通知尾部、整批先注销、第二次 request_stop 不充当 join |
| 14.3 恢复调度 | 真实公开 sender 和默认 task 的 0/1 路径、保存资源、原 token 取消、排队取消、先撤责后宿主跳转、完整签名、公开 connect 失败、两种非法环境拒绝、6 条内部错误终止路径 |
| 14.4 fork／task | 固定/动态 fork 分配失败、task 创建失败、嵌套责任、child 取消后的 frame/record 保留、parent 重入、外部 error/stopped 传播、optional 宿主 value/error/stopped、延迟释放反例及安全排空、独立取消、通知/观察线程收尾 |
| 14.5 工具 | MSVC、多翻译单元、ASan、TSan、既有状态模型及组件探针；各批次日志独立统计 |

测试对 callback 插桩副本核对固定头 SHA256 和插入次数，未修改原 stdexec
checkout。暂停通知尾部的阻塞 notifier 是受控测试注入，不是支持生产阻塞通知器。
新测试树复用 `instrument_stop_token.cmake` 的校验和插桩逻辑；重构后 Windows
生成头 SHA256 未变，两组 hook CTest 再次通过。Linux 生成结果仅换行格式不同，
归一化后文本相同，CMake 脚本和 shell 语法检查也通过。

CountingScheduler 透明包装真实 run_loop，并隔离固定 stdexec Debug 查询中
用于断言的未启动 schedule sender 构造。测试分开统计 schedule 工厂调用和
operation start，扣除宿主启动基线；不把库诊断构造当作执行跳转。

固定 `when_all` 的取消转发可能由请求停止线程完成最后一次 arrive，从而在该
线程提交嵌套组合的最终 stopped。测试不将外层组合的线程行为误当作 Pause
本体的承诺；直接等待 Pause 的正常 continuation 和宿主 stopped 资源断言保留。

Source、stop source、scheduler 及通知资源的外部静默期仍由宿主保证；计数归零
不等于全部调用栈退出。首版后端仍限经审查的 run_loop 和默认 task allocator。
有限压力、ASan 和 TSan 不能证明所有可能交错或无限嵌套深度。

## Debug / Release 测试与 clice 补充验收

用户要求同时验证两种宿主构建模式后，增加继承公共测试配置的
`msvc-debug`／`msvc-release` configure、build、test presets。
`scripts/msvc-cmake.cmd test` 现在默认依次执行两套完整测试，也支持指定
`debug`、`release` 或 `all`。不增加库构建制品，不修改 `pause.hpp`。

正负环境编译测试同步继承每个配置的 flags 和 CRT：Debug 实测为
`/Od /RTC1 /MDd`，Release 为 `/O2 /DNDEBUG /MD`。所有验收 CHECK 在
Release 中仍执行。旧 `stdexec_standard_task_scheduler_test` 曾把宿主
schedule 工厂调用数固定为 2；Release 暴露该假设。核对固定 stdexec
源码后，区分 Debug 断言专用的额外 sender 构造，其他路径共用已校验的
宿主基线；真实 schedule operation 启动数和 affinity 要求保持不变。
此前该组件的 ASan 350,000 场景属于修改前的测试源码，本次修改后的
测试分别由两种配置的全套 CTest 重新验证。

| 本次补充测试 | Debug | Release |
| --- | ---: | ---: |
| 全套 CTest | 19/19，25.52 秒 | 19/19，12.02 秒 |
| integration，10,000 轮 + 4 条深链 | 200,004 | 200,004 |
| sender，10,000 轮 | 270,000 | 270,000 |
| 确定性 callback hook，10,000 轮 | 70,000 | 70,000 |
| task lifetime，10,000 轮 | 30,000 | 30,000 |
| 四组压力合计 | **570,004** | **570,004** |

压力共 **1,140,008** 个真实实现场景，8 个独立进程均退出 0、无超时、
stderr 为空，记录的源码／头文件／二进制哈希在运行前后相同。共 80,000 次
争用完成均恰好一次；随机 fork/pause 与 parent/reentry 各 20,000 轮；
两种配置均通过 32/64 层真实链。这一批未开启 sanitizer，不混入上文的
ASan、TSan 或模型计数。完整日志为 `build/debug-release-ctest-final.log`，
压力参数、逐进程结果及哈希在 `build/debug-release-stress/`。

`.clice/config.toml` 按官方文档声明两种配置，使用 `../build/` 下的相对路径
读取各自 CMake 编译数据库；根目录不再保留优先级更高的 `clice.toml`。
本机 clice `0.1.0+g900128f` 的独立 inspect 已在两种配置下解析真实
`pause.hpp`，均无 error／diagnostics；它从 `tests/pause_header_compile.cpp`
选择 header 上下文，自动查询 MSVC，且使用的 Debug／Release 编译参数
哈希不同。日志与 JSON 在 `build/clice-validation/`。
实际隔离 LSP 验证发现该版本生成 PCH 时内置 Clang 在 `ExprCXX.h:2957`
断言失败，导致 worker 崩溃及 `Document is quarantined`；单凭 inspect
符号解析成功不能声称补全正常。按官方配置启用 `[project] readonly = "on"`
后，Debug／Release 各两次真实成员补全全部通过（`snapshot`、`suspend_point`），
两个独立服务正常退出，用户原有服务和磁盘源码未改。
隔离覆盖实验的证据为 `build/clice-validation/lsp-readonly-results.json`。
设置写入根 `clice.toml` 后，去除初始化参数中的 readonly 覆盖，再次从
实际配置文件验证两种模式的四次补全，全部通过；最终证据为
`build/clice-validation/lsp-project-config-results.json` 及对应 transcript／日志。
随后按用户要求迁移至 `.clice/config.toml`，调整编译数据库相对路径并移除
根配置，再次验证 Debug／Release 的四次实际补全，全部通过；迁移后的证据为
`build/clice-validation/lsp-dot-clice-config-results.json`。
该模式不限制编辑与构建，但不复用 PCH，补全实测每次约 2.6–2.7 秒，
其他查询主要依赖索引；项目保留默认后台索引。隔离补全验证为避免改动用户
索引而关闭了后台索引，未验证所有索引功能或源码实时 LSP 诊断；两配置的
源码编译健康由前述 inspect 检查。升级 clice 后可再核对 PCH 并移除兼容项。
已启动的编辑器服务须执行 `Clice: Restart Language Server` 读取新配置；
`Clice: Switch Build Configuration` 用于切换智能提示上下文。

## 复现

普通构建和真实／设计全套测试：

```powershell
.\scripts\msvc-cmake.cmd test
```

在 x64 Visual Studio Developer PowerShell 中执行 ASan；脚本逐进程设置超时，
记录构建、输出、退出码和哈希：

```powershell
.\scripts\run-pause-stress.ps1 -Iterations 10000 -ModelSteps 100000 -ModelSeeds 100
.\scripts\run-pause-stress.ps1 -OnlyTest pause_resource_test -Iterations 10000
```

Linux/WSL 的测试专用 TSan（需先有固定 stdexec 源码缓存）：

```sh
bash scripts/run-pause-tsan.sh --iterations 1000
```

## 修改范围

- 生产实现：`pause.hpp`。
- 构建／多 TU：`CMakeLists.txt`、`tests/pause_header_compile.cpp`、`tests/pause_header_other.cpp`。
- 两种宿主测试配置及智能提示：`CMakePresets.json`、`scripts/msvc-cmake.cmd`、
  `.clice/config.toml`；环境编译脚本继承当前 flags/CRT，旧 scheduler 探针修正 Debug 专用基线。
- 真实验收：六个 `pause_*_test.cpp`，共享 `pause_sender_test_support.hpp`，
  `pause_environment_compile.cpp`、`check_pause_environment.cmake`、`instrument_pause_stdexec.cmake`。
- 插桩复用：`tests/instrument_stop_token.cmake` 加入 include guard，供新旧测试共用。
- 可复现压力：`scripts/run-pause-stress.ps1`、`scripts/run-pause-tsan.sh`。
- 说明：`pause-branch-design.md` 的实现状态、`tests/README.md` 及本报告。
  按用户确认补充 STL noexcept 内部失败的终止边界；模型未改写，固定依赖未升级。

无持久化格式或数据迁移，回滚仅涉及源码与构建配置。STL 迭代器级别保留
默认值；本次边界确认没有修改生产实现、公共 API 或编译配置。
