# Pause

基于 NVIDIA stdexec 的 C++23 头文件库，为协程工作流提供**协作式暂停、整体暂停屏障和单步推进**。
适合批处理、数据导入、分块计算和并行请求等需要在明确业务边界暂停的任务。
库与命令行示例使用 ISO C++23，唯一的第三方 C++ 依赖是 NVIDIA stdexec（含其 `exec` 扩展头文件）。

```cpp
#include <pause.hpp>
// C++ namespace: coop_pause
// CMake target:  pause::pause
```

暂停不会抢占正在执行的业务。每个参与分支必须到达暂停点或退出，整体状态才会进入
`Paused`。宿主负责执行器、取消传播、任务组合以及资源的最终释放。

## 快速开始

当前维护基线为 **Windows x64、MSVC、C++23、CMake 和 Ninja**。
自动获取固定 stdexec 提交时需要 **CMake 3.28+、Git 和网络**；复用已安装依赖时，
Pause 自身要求 CMake 3.25+。其他平台和协程宿主的支持范围见[使用指南](docs/user-guide.md)。

在 VS x64 Developer PowerShell 中，从仓库根目录执行：

```powershell
cmake --preset msvc-release -DPAUSE_BUILD_EXAMPLES=ON
cmake --build --preset msvc-release --parallel 2
ctest --preset msvc-release
.\build\msvc-release\pause_simple_workflow.exe
```

也可从普通终端运行 `scripts\msvc-cmake.cmd test all`，自动初始化 MSVC 并执行
Debug/Release 测试。首次配置会下载 stdexec 及其配置期辅助工具。
统一入口还支持 `examples debug` 构建调试示例、`release` 执行完整发布验证。

从[简单示例](examples/cli/simple_workflow.cpp)开始：一个业务协程执行三批工作，
独立控制线程发出暂停、单步和继续命令。其余三个短例子分别展示线程池分支、取消和
普通 sender 接入，见[下方示例](#示例)。

## 在应用中使用

安装开发包后：

```cmake
cmake_minimum_required(VERSION 3.25)
project(my_app LANGUAGES CXX)
find_package(pause 0.1 CONFIG REQUIRED)
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE pause::pause)
```

配置时通过 `CMAKE_PREFIX_PATH` 指向安装前缀。也支持 `add_subdirectory` / FetchContent
集成；作为子项目时，维护测试和工具链检查默认关闭。
依赖优先复用 `STDEXEC::stdexec` 或已安装的 stdexec 包，缺失时获取固定提交
`a23aa7abc8796edf158ff67afaf732a6dad3e2a5`。
该提交包含 `when_all` 完成/取消竞争修复；配置会拒绝已知存在此竞态的旧实现。
使用 `PAUSE_FETCH_STDEXEC=OFF` 可禁止自动下载；详细接入步骤见[使用指南](docs/user-guide.md)。

| 对象 | 职责 |
| --- | --- |
| `coop_pause::PauseSource` | 共享暂停状态、控制命令和快照；地址必须稳定 |
| `coop_pause::PauseBranch` | 一条串行路径的暂停责任；并发工作使用独立 branch |
| stdexec 宿主 | 调度、取消、完成通知，以及协程和 operation 的生命周期 |

`PauseSource(mode, notifications = {})` 接受三个可选阶段回调；只在完整事务的最终阶段
改变时通知，同阶段计数或 epoch 更新不通知。最后一个直接 child 退役时自动归还 parent
责任，继续业务前仍须经过安全暂停点。`resume(mode)` 可尝试恢复本次调用观察到的暂停轮次，
也保留显式 `resume(epoch, mode)` 绑定轮次的入口。

默认从唯一 root fork 出并行责任。`make_branch()` 仅在预处理宏
`PAUSE_ENABLE_INDEPENDENT_BRANCH=1` 时公开；宏默认值为 0，所有包含本头文件的翻译单元
必须取同一值。它不是下表中的 CMake 选项。

## 构建选项

| 选项 | 独立构建默认值 | 用途 |
| --- | --- | --- |
| `PAUSE_BUILD_TESTS` | `ON` | 真实实现与安装管理回归，绑定固定 stdexec |
| `PAUSE_BUILD_TOOLCHAIN_SMOKE_TEST` | `ON` | 工具链和公共头文件编译检查 |
| `PAUSE_BUILD_DESIGN_TESTS` | `OFF` | 设计模型及固定依赖契约测试；MSVC 测试 presets 启用 |
| `PAUSE_BUILD_EXAMPLES` | `OFF` | 命令行示例及静态线程池案例；MSVC 测试 presets 启用 |
| `PAUSE_INSTALL` | `ON` | 安装规则和 ZIP/TGZ 开发包 |
| `PAUSE_FETCH_STDEXEC` | `ON` | 找不到已安装依赖时自动获取 |

## 示例

| 入口 | 内容 |
| --- | --- |
| [simple_workflow.cpp](examples/cli/simple_workflow.cpp) | 入门：请求暂停、单步一次、连续完成三批业务 |
| [thread_pool.cpp](examples/cli/thread_pool.cpp) | 原生线程池：两条分支、逐轮屏障与父分支汇合 |
| [cancellation.cpp](examples/cli/cancellation.cpp) | 暂停时请求 stop，业务不继续，完成结构化释放 |
| [sender_workflow.cpp](examples/cli/sender_workflow.cpp) | 无业务协程的 sender pipeline 与 branch 借用期 |

命令行示例仅依赖库已有的 stdexec，按[快速开始](#快速开始)构建后，
可用 `ctest --preset msvc-release -R '^pause_example_'` 单独运行示例测试。
详细参数和预期行为见[工作流详解](docs/workflows.md#工作流案例)。
`example_support.hpp` 属于示例宿主，不属于公共 Pause API。

线程池案例可运行 `pause_thread_pool.exe 4`；`pause_thread_pool.exe 1`
验证单 worker 下的协作式收尾。每条并行任务显式使用原生 scheduler 的 `starts_on`。
应用中的接入方式和调度器要求见[调度器接入](docs/user-guide.md#调度器接入)。

> **调度器选择注意：** 当前默认 task 直接使用 run_loop 或静态线程池的原生 scheduler。
> `stdexec::parallel_scheduler` 不满足当前完成契约，不通过屏蔽错误的包装强行接入。
> 大量 `when_all` 的并行方式、分配开销及资源生命周期，见[使用注意事项](docs/user-guide.md#调度器接入)。

## 项目结构

| 位置 | 用途 |
| --- | --- |
| `pause.hpp` | 公共单头文件库 |
| `cmake/`、`CMakeLists.txt`、`CMakePresets.json` | 依赖检查、构建、安装与打包 |
| `examples/cli/` | 命令行示例与业务流程 |
| `examples/common/` | 命令行示例共用的宿主辅助代码 |
| `tests/runtime/` | Pause 实现、并发、异常和生命周期回归 |
| `tests/design/` | 状态模型与固定 stdexec 契约 |
| `tests/compile/` | 公共头文件、工具链及合法/非法环境编译检查 |
| `tests/packaging/`、`tests/repository/` | 包集成、安装管理和仓库检查回归 |
| `scripts/`、`tests/scripts/` | 日常构建、发布检查及专项测试脚本 |
| `docs/` | 使用、工作流、设计和打包说明 |
| `.github/`、`.vscode/`、`.clice/` | CI、协作模板和编辑器配置 |
| `build/` | 被 Git 忽略的构建产物和验证记录 |

## 文档与维护

| 内容 | 入口 |
| --- | --- |
| API、错误处理、取消与安全关闭 | [使用指南](docs/user-guide.md) |
| 源码接入与完整工作流案例 | [工作流详解](docs/workflows.md) |
| 状态机、并发协议与生命周期约束 | [设计契约](docs/design.md) |
| 构建、安装、更新和卸载 | [打包说明](docs/packaging.md) |
| 测试命令与验收范围 | [测试指南](tests/README.md) |
| 调试器和编辑器 | [开发环境](CONTRIBUTING.md#开发环境) |
| 发布步骤和产物核验 | [发布指南](CONTRIBUTING.md#发布) |
| 贡献规范与变更记录 | [CONTRIBUTING](CONTRIBUTING.md)、[CHANGELOG](CHANGELOG.md) |

## 许可证

本项目采用 **Apache-2.0 WITH LLVM-exception**（Apache 2.0 附 LLVM 例外），
与当前 LLVM 项目使用的许可方案一致。完整条款见 [LICENSE](LICENSE)，
第三方依赖仍遵循各自的许可证，见[第三方说明](THIRD_PARTY_NOTICES.md)。
