# 本地开发与编辑器配置

本文收纳仓库开发环境的设置，命令从仓库根目录执行。路径、工具版本和调试结果中标为“本机”的内容属于已记录的环境，其他机器须按实际安装调整。库的接入要求见 [README](../README.md#1-支持环境与构建)。

## 1. 用 LLDB DAP 调试 examples（本机 Windows 配置）

仓库已提供 `.vscode/launch.json`、`tasks.json`、`settings.json` 和扩展推荐，适用于 VS Code/Cursor。本机已有官方 `llvm-vs-code-extensions.lldb-dap` 扩展。打开运行和调试面板，选择 **Pause: simple workflow (LLDB DAP)**，按 **F5** 即可在 `main` 停下。简单示例没有参数，可在 `work_task` 的 `3. validate: begin`、`4. fetch: begin`、`when_all returned` 日志处，以及 `control_task` 的 `source.request_pause`、`source.resume` 处设置断点。

F5 前的任务会运行 `scripts/build-debug-examples.cmd`，自动初始化 VS x64 编译环境、启用 examples 并构建四个示例。`msvc-debug` preset 使用 `/Zi /Od` 和 `/DEBUG:FULL /INCREMENTAL:NO`，生成与 exe 配套的完整 PDB。其余三个入口分别调试综合示例、算法示例和动态任务树；参数通过下拉框选择。

调试综合示例时，可选择 **Pause: control + work task (LLDB DAP)**，在 [work_task](../examples/controlled_business.hpp) 的 `work: begin controlled workflow` 语句、`load_source`、`compute_shard` 和 [control_task](../examples/controlled_workflow.cpp) 中设置断点，然后按 F5 继续。**F10** 逐过程、**F11** 逐语句、**Shift+F5** 停止调试。综合示例参数 `step` 控制 Pause 的业务轮次，F10/F11 控制调试器的源码步进；跨 `co_await` 挂起/恢复时可能进入 stdexec 帧，可在恢复后的业务语句设置断点。

工作区默认从 `PATH` 查找 `lldb-dap`。请安装并验证可用的 LLDB DAP 适配器；如需指定安装位置，可在本机编辑器设置中覆盖 `lldb-dap.executable-path`。使用依赖 Python 的适配器时，按该发行版要求配置运行环境。

历史真实 DAP 会话已验证综合示例的 PDB 加载、`main` 断点、`controlled_business.hpp` 第 68 行断点、`next` 到第 70 行以及退出码 0，日志为 `build/lldb-dap-validation.json`。上述行号属于当时源码，修改业务后应按语句重新定位断点。本机 MSVC 协程恢复帧在 LLDB 中可能显示地址而非函数名，局部变量视图可能为空；这次验证不代表完整的协程变量可视化支持。

## 2. examples 的 clice 代码提示

[.clice/config.toml](../.clice/config.toml) 的 `debug` / `release` 配置优先读取
`build/qt-debug` / `build/qt-release` 中的编译数据库，包含普通示例和 Qt 示例，
并保留 `build/msvc-debug` / `build/msvc-release` 作为测试编译条目的备用来源。
Qt 数据库提供正确的头文件路径和 `QT_NO_KEYWORDS` 等宏；仅使用未开启
examples 的测试数据库，会使示例借用错误的编译命令，影响补全。

在 VS x64 Developer PowerShell 中运行 `cmake --preset local-qt-debug` 或
`cmake --preset local-qt-release` 即可生成数据库，无需先编译程序。
其他机器需要先在本机 CMakeUserPresets.json 中设置 Qt 路径，并启用
`PAUSE_BUILD_EXAMPLES`、`PAUSE_BUILD_QT_EXAMPLE` 和 `CMAKE_EXPORT_COMPILE_COMMANDS`。

根据 [clice 配置文档](https://docs.clice.io/zh/clice/guide/configuration)，
配置中的相对路径以配置文件所在目录为基准，因此这里使用 `../build/...`。
配置只在启动时读取；修改后执行 **Clice: Restart Language Server**。
可用 **Clice: Switch Build Configuration** 切换 Debug/Release，或用
**Clice: Show Current Compilation Context** 检查当前文件采用的编译命令。
示例头文件从包含它的源文件继承编译上下文。配置使用 `readonly = "off"`，
使独立打开、尚未建立索引的 `.hpp` 也能按需构建完整 AST，提供悬停、
符号和语义提示。旧版 clice 的 `readonly = "on"` 兼容设置已移除；
它只从索引回答这些查询，不能保证冷启动头文件的解析。
本机 clice `g1b5d0f3` 在从预编译头重载部分 stdexec 类型时仍会崩溃；
Qt 示例在重型 include 之前保留类型声明，限制预编译前缀的范围，
使业务头文件正常解析。依赖 UI 内部类型的离屏验收代码已并入 Qt `.cpp`，
避免以不完整的头文件上下文单独解析。
