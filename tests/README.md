# 测试指南

本目录保留当前维护的 C++ 回归源码、配置期探针、包集成测试及运行脚本。
设计要求见[设计契约](../docs/design.md)，测试中的断言和参数定义当前覆盖范围。

当前测试使用 `PausePhaseNotifications` 和最后 child 退役时的 parent 责任归还语义。
默认 API 由 `pause_phase_test` 和多翻译单元编译检查覆盖；需要独立顶层分支的测试在
各自单翻译单元中显式启用扩展，环境编译检查同时验证宏值 0 和 1。

2026-11-07 在 Windows x64 / MSVC 上执行：Debug、Release 各 39 项 CTest 全部通过；
原生调度器 ASan 检查通过 run_loop 和 2 workers 各 10 轮，另通过线程池 1 worker 的
10 轮、4 workers 的 100 轮。记录仅适用于当前源码、固定 stdexec 和这些参数；TSan 入口
已同步，未在本轮 Windows 环境运行。

当前回归重点：

- 按完整事务的阶段转换核对 `PausePhaseNotifications` 通知，
  包括空 SingleStep 的新 epoch 无通知、同阶段计数无通知、直接 Open→Paused 只通知 Paused。
- 区分默认唯一 root 接口与启用 `PAUSE_ENABLE_INDEPENDENT_BRANCH=1` 的扩展测试；
  同一可执行程序所有包含 Pause 的翻译单元宏值须一致，并验证默认不公开 `make_branch()`。
- 状态模型与真实父子生命周期验证：最后 child 在 Open/Collecting/Paused 中退役时自动
  归还 parent；Paused 中旧 epoch 失效；连续再次 fork、parent 取消或直接退出均正确清理。
- 验证 `resume(mode)` 的错误顺序及两次加锁之间的竞争失败，不自动重试。
- 验证 Open 路径不查询/复制 scheduler、等待后恢复查询启动 scheduler，保留原有取消、
  调度亲和及完整 operation 生命周期回归。

## 目录分类

| 目录 | 内容 |
| --- | --- |
| [runtime/](runtime/) | 实现回归及共享 sender 测试辅助头 |
| [design/](design/) | 状态模型与 stdexec callback、task、scheduler 契约 |
| [compile/](compile/) | 工具链、公共头文件及环境编译检查 |
| [packaging/](packaging/) | 安装管理、真实包集成和消费者工程 |
| [repository/](repository/) | 仓库与文档发布检查的 Python 回归 |
| [scripts/](scripts/) | 压力测试入口及固定依赖插桩 |
| [support/](support/) | 按阶段触发的测试通知辅助函数 |

库配置时必需的 stdexec 兼容性探针位于 `cmake/check_stdexec.cpp`，随开发包分发。

## 日常回归

从仓库根目录运行，可自动初始化 VS x64 编译环境：

```powershell
scripts\msvc-cmake.cmd test all
```

或在 VS x64 Developer PowerShell 中选择一个配置：

```powershell
cmake --preset msvc-release -DPAUSE_BUILD_EXAMPLES=ON
cmake --build --preset msvc-release --parallel 2
ctest --preset msvc-release
```

用 `msvc-debug` 替换配置名即可运行 Debug。
公开 MSVC presets 启用实现、设计、工具链和命令行示例测试。

## 覆盖范围

| 分组 | 开关 / 入口 | 范围 |
| --- | --- | --- |
| 实现回归 | `PAUSE_BUILD_TESTS` | 暂停、sender、分支、取消、并发、资源、异常和生命周期 |
| 工具链检查 | `PAUSE_BUILD_TOOLCHAIN_SMOKE_TEST` | 公共头多翻译单元及 stdexec 编译/运行 |
| 设计与依赖契约 | `PAUSE_BUILD_DESIGN_TESTS` | 状态模型、stop callback、task affinity 和有限深链 |
| 命令行示例 | `PAUSE_BUILD_EXAMPLES` | 暂停/单步、线程池分支、取消和普通 sender pipeline |
| 安装管理 | `pause_package_manager` | 所有权、更新/卸载、修改拒绝、路径及事务边界 |
| 真实包集成 | `packaging/package_integration_test.cmake` | ZIP/TGZ、搬移、离线消费者及随包示例 |

实现插桩和部分契约断言绑定默认 stdexec 提交。使用其他依赖版本时，应关闭这两组
测试，重新完成应用所需的兼容性验证；配置检查通过不能代替生命周期与并发回归。

主要契约与测试源码的对应关系：

| 契约 | 测试文件 |
| --- | --- |
| 默认 API、阶段通知及父责任归还 | `runtime/pause_phase_test.cpp`；包含 Open 零 scheduler 查询、等待后才读取 scheduler、便利 resume 与空屏障 |
| 状态与暂停责任 | `runtime/pause_integration_test.cpp`、`design/pause_state_model_test.cpp` |
| 原生 run_loop 与单/多 worker 线程池 | `runtime/pause_scheduler_test.cpp`；覆盖原始 sender、task、单步、独立取消、异常及恢复/取消竞争 |
| sender 完成、取消和确定性交错 | `runtime/pause_sender_test.cpp`、`runtime/pause_sender_hook_test.cpp` |
| 并发、批次、状态转换及固定深度 | `runtime/pause_concurrency_stress_test.cpp`、`runtime/pause_batch_extreme_test.cpp`、`runtime/pause_transition_extreme_test.cpp`、`runtime/pause_extreme_test.cpp` |
| 分配失败及资源回滚 | `runtime/pause_resource_test.cpp` |
| task、外层 operation 及通知尾部的生命周期 | `runtime/pause_task_lifetime_test.cpp`、`runtime/pause_outer_lifetime_test.cpp`、`runtime/pause_notification_matrix_test.cpp` |
| 异常、宿主失败及预期终止 | `runtime/pause_exception_matrix_test.cpp`、`runtime/pause_host_failure_test.cpp`、`runtime/pause_sender_death_test.cpp`、`runtime/pause_contract_death_test.cpp` |
| stdexec callback、task、scheduler 和有界深链 | `design/stdexec_*_test.cpp` |

## 发布验证

原生调度器回归覆盖普通 connect/start、`pause_sender | then(...)`、task、固定/动态 fork、连续和单步、独立取消、预取消、业务异常、控制异常及恢复/取消竞争。

多 worker 专项用线程池原生 `get_scheduler_on_thread(0/1)` 安排两条测试路径，以 latch 确认不同 worker 同时执行业务，再验证 Collecting、双分支 Paused、单步及父分支汇合。这里的阻塞闸门仅用于已指定两个 worker 的测试；1 worker 和入门示例不会阻塞业务 worker 等待控制命令。

原生调度器回归可单独运行：

```powershell
ctest --preset msvc-debug -R '^pause_scheduler_' --output-on-failure
.\build\msvc-debug\pause_scheduler_test.exe --backend loop --iterations 100
.\build\msvc-debug\pause_scheduler_test.exe --backend pool --threads 1 --iterations 100
```

CTest 覆盖原生 run_loop 和 1/2/4 worker 静态线程池，直接使用其 scheduler；控制等待不占用池内线程。
同一份源码在 Debug/Release 上验证，取消和异常后要求全部分支、协程局部资源排空。
每轮还强制 `when_all` 的最后一个子任务与已选中、尚未执行的取消回调交错，验证只完成一次。
默认 stdexec 已更新到取消竞争修复提交 `a23aa7abc8796edf158ff67afaf732a6dad3e2a5`；
对应 stop-token 插桩重新核对，首次 `request_stop()` 返回 `true`，重复请求返回 `false`。

仓库检查的回归无需 C++ 编译器：

```powershell
python -m unittest discover -s tests/repository -p "test_repository_checks.py"
```

普通 Windows 终端运行：

```powershell
scripts\msvc-cmake.cmd release
```

该入口执行源码检查、实现/设计/示例测试、独立 stdexec 安装及真实包集成。
`scripts\msvc-cmake.cmd test debug` 执行 Debug 构建和测试。输出均位于 `build/`，不向系统安装依赖。
完整前提及产物位置见[发布指南](../CONTRIBUTING.md#发布)。

安装管理回归也可独立执行，无需编译器：

```powershell
cmake -DOUTPUT_DIRECTORY="$PWD/build/package-manager-tests" -P tests/packaging/package_manager_test.cmake
```

## 专项压力测试

这些脚本增加完整场景的重复次数，不属于常规 CI：

| 入口 | 用途与前提 |
| --- | --- |
| [run-pause-release-stress.ps1](scripts/run-pause-release-stress.ps1) | 重复执行 Release 二进制；先运行 `scripts\msvc-cmake.cmd test release` |
| [run-pause-stress.ps1](scripts/run-pause-stress.ps1) | 另行编译并运行 MSVC ASan；先运行 `scripts\msvc-cmake.cmd test debug`，然后在 VS x64 开发终端执行 |
| [run-pause-tsan.sh](scripts/run-pause-tsan.sh) | Linux/WSL TSan；需要支持 C++23/TSan 的编译器和固定 stdexec 源码，参数见 `--help` |

Windows 小量验证可用 `-OnlyTest pause_sender_test -Iterations 2`；线程池 ASan 检查可用
`-OnlyTest pause_scheduler_test -Iterations 10`。TSan 的 `--suite scheduler` 运行同一份原生
调度器源码，`--suite phase` 运行默认 API 与阶段回归；完整默认重复量很大。
按脚本参数选择输出目录，日志和运行结果写入 `build/`。运行次数只适用于对应源码、
依赖、编译器及具体参数，不能当作所有平台或所有线程交错均已验证。
