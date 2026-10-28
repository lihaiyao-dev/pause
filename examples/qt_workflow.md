# Qt 工作流 UI 接入

UI 提供单向分支图、暂停、继续、单步、取消、重启、节点耗时与进度、概览和缩放。业务流程由你组合；默认只保留一个长短路径的最小预览，便于直接运行界面。

## 修改业务的入口

编辑 [qt_workflow.hpp](qt_workflow.hpp) 中的 `business()` 协程函数，使用 `co_await`、条件、循环和 `when_all` 编写自己的业务控制逻辑。它接收借用的 `RunContext&` 与根 `Branch&`，返回 `stdexec::task<void>`。

`make_layout()` 单独描述显示关系。`make_workflow()` 只绑定二者：`return {make_layout(), business};`。UI 创建 Session 后显式调用 `start()`，由 Session 构建并运行这次业务协程及其执行资源；业务函数只负责本次执行的业务流程。

下面的布局对应“入口 → 两条并行路径 → 输出”：

```cpp
sequence({
    step("input", "入口"),
    parallel("paths", "并行", {
        step("short", "短路径"),
        sequence({step("a", "长路径第一步"), step("b", "长路径第二步")})}),
    step("output", "输出")})
```

业务入口是普通协程函数。将你的 sender 传给 `run.step`，对应的 key 必须存在于布局中；需要管道组合时，仍可使用原生 stdexec 表达式：

```cpp
stdexec::task<void> business(RunContext& run, Branch& root)
{
    auto data = co_await run.step(root, "input", load_data());
    co_await run.group(root, "paths", 2, [&](auto& branches) {
        return stdexec::when_all(
            short_path(run, std::move(branches[0]), data),
            long_path(run, std::move(branches[1]), data))
            | stdexec::then([](auto a, auto b) { merge_results(a, b); });
    });
    co_await run.step(root, "output", publish());
}
```

这里的 `load_data`、`short_path`、`long_path`、`merge_results` 和 `publish` 由你实现。路径函数按值接收 Branch，并返回自己的 task；串行步骤借用这条路径的 branch，例如：

```cpp
stdexec::task<int> long_path(RunContext& run, Branch branch, const Data& data)
{
    int value = co_await run.step(branch, "a", compute_a(data));
    co_return co_await run.step(branch, "b", compute_b(value));
}
```

Branch 必须覆盖路径的完整生命周期。各并行路径通过移动取得自己的 Branch，完成后释放暂停责任，短路径不会阻碍长路径的下一次暂停。不要让两个并发路径共用一个 Branch，也不要把借用 Branch 的 sender 留到它析构后执行。

## 已封装的函数

| 接口 | 作用 |
| --- | --- |
| `run.step(branch, key, sender, planned_ms = 0)` | 在入口等待安全点；放行后运行 sender，显示运行、完成、error、stopped 或取消状态；转发原有返回值，包括多个值和只能移动的对象 |
| `run.group(parent, key, count, build, selection = RunContext::All)` | 接入分叉/汇合安全点，转交子路径责任，调用你提供的 sender 组合；显示未选路径已跳过和汇合等待 |
| `run.annotate(group_key, text)` | 给汇合节点补充你定义的结果说明 |
| `run.timer()` | 提供定时器的 scheduler，可用于自定义异步等待 |
| `run.delay_step(branch, key)` | 可选预览占位步骤，独立随机生成等待时长；真实业务接入用 step |

`group` 的 build 接收子 Branch 的 vector，返回你组合的 sender。其结果需要在 build 内的 `then/let_value` 中处理；group 本身返回 `task<void>`。`count` 须等于选中的布局子路径数量。selection 可为 All、Skip 或单个子路径索引；空组不调用 `fork(0)`。

布局函数还提供 `optional`、`choose`、`race`、`recover_stopped` 和 `on_second`，用于显示条件、竞速、停止恢复及执行器切换。它们只描述图形。相应的业务条件、`when_any`、停止恢复和调度切换由你用 stdexec/exec 自行组合；需要保持布局中的选中状态与实际执行一致。

图布局是显式元数据，无法从任意 sender 自动反射。业务步骤通过 key 对应 UI；改变组合关系时，同步调整布局。

## 控制与时间显示

Session 持有一个 PauseSource，Qt 按钮调用 `pause()`、`advance(Continuous)`、`advance(SingleStep)` 和 `cancel()`。暂停/继续/单步直接控制 PauseSource，取消使用独立 stop source。UI 按 Source 快照更新按钮，不另建暂停状态机。

UI 管理整个会话的生命周期：创建 Session → start 构建并运行新的业务协程 → 控制 PauseSource → 请求取消 → 等待根协程、子任务和执行资源全部退出 → 销毁 Session。重启先释放旧会话，再创建新会话。协程帧由 task/operation 的所有权封装自动释放，不能在仍有异步操作使用它时直接 destroy；业务协程无需管理这些宿主资源。

暂停在安全点生效，不抢占正在执行的步骤。单步放行当前所有等待分支，各自推进到下一安全点；分叉和非空组汇合也有安全点。关闭或重启先取消并等待实际执行资源退出，再释放或重新创建会话。

真实 sender 默认仅显示实际耗时。传入大于 0 的 planned_ms 后显示预计时长与进度，完成前最多 99%，不会额外插入延时。默认预览每次构造步骤独立随机抽取 400–8000ms；暂停和继续不重抽。重启重建整个运行流程。

节点计时不包含自身入口的暂停等待。汇合计时包含等待子路径运行和暂停的墙钟时间。“本次放行耗时”统计放行命令到下一屏障收敛或工作流结束的时间。界面约每 33ms 刷新。

## 文件组织

- [qt_workflow.hpp](qt_workflow.hpp)：你修改的 business 协程与独立图布局，默认是最小预览。
- [qt_workflow_support.hpp](qt_workflow_support.hpp)：布局、状态快照、RunContext 和 Session 封装。
- [qt_workflow.cpp](qt_workflow.cpp)：Qt 控件、状态更新、程序入口及内部离屏验收。

内部验收依赖本文件中的 UI 类型，保留在同一个翻译单元，避免头文件缺少独立解析上下文。

## 本机构建

需要 C++23、兼容的 stdexec 和 Qt 6.5+ Widgets。本机 CMakeUserPresets.json 已设置 MSVC x64、Qt/Ninja 路径和 `PAUSE_BUILD_QT_EXAMPLE=ON`，按 .gitignore 保留为本地配置。在 VS x64 Developer PowerShell 中：

```powershell
cmake --preset local-qt-debug
cmake --build --preset local-qt-debug
ctest --preset local-qt-debug --parallel 3
# Set QT_ROOT to the Qt kit installed on this machine.
$env:PATH = (Join-Path $env:QT_ROOT 'bin') + ';' + $env:PATH
.\build\qt-debug\pause_qt_workflow.exe
```

Release 使用 local-qt-release，程序位于 build/qt-release。本机测试 preset 仅运行 Qt 测试，构建 preset 仅构建 Qt 示例。直接运行时需将 Qt bin 加入 PATH，或用 windeployqt 部署运行库和平台插件。

Qt 开关默认关闭，与 PAUSE_BUILD_EXAMPLES 独立；关闭时不查找 Qt，示例不进入开发包安装清单。

15 项离屏验收覆盖暂停/继续/单步、长短路径汇合、取消、关闭/重启清理、自定义图、同步和异步 sender、多个返回值、只能移动的值、error/stopped、启动前取消和空组合。测试预览使用 80–240ms 随机等待。

```powershell
.\build\qt-debug\pause_qt_workflow.exe --smoke-test controls --screenshot .\build\qt-workflow.png
```

此命令在短路径完成、长路径仍等待的状态保存窗口截图，并生成 qt-workflow.png.graph.png 整图。
