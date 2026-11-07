# Changelog

这里记录面向使用者的改动。版本号以根目录 `CMakeLists.txt` 为准。
0.x 阶段只在同一 minor 内提供 CMake 包版本兼容，不承诺跨 minor 的 API 兼容。

## [Unreleased]

### Added

- `PausePhaseNotifications` 的三个可选阶段回调，以及读取当前暂停轮次的 `resume(AdvanceMode)` 便利重载。
- 原生 run_loop、1/2/4 worker 静态线程池的直接接入回归，以及普通 sender 组合、真实双 worker 并行、运行中暂停和单步验证；不增加公共 scheduler 包装。
- 项目维护文档、GitHub CI、仓库检查和发布验证入口。
- 开发包中的变更记录与第三方依赖说明。
- `Apache-2.0 WITH LLVM-exception` 许可证及随包许可文本。

### Changed

- `PauseSource` 的第二个参数改为 `PausePhaseNotifications`，默认 `{}`；替代原单个 `StateChangedNotifier`，只通知完整事务最终进入的新阶段，不通知同阶段计数、revision 或 epoch 变化。
- 最后一个直接 child record 退役时自动归还 parent 责任；Paused 中归还会重新进入 Collecting 并清空旧 epoch。parent 可在 children 退役后再次 fork，继续业务前仍须经过暂停点。
- `make_branch()` 改为默认关闭的扩展，通过所有翻译单元一致的预处理宏 `PAUSE_ENABLE_INDEPENDENT_BRANCH=1` 启用；默认使用唯一 root 的责任树。
- Open 暂停点直接通过时不再读取或复制 scheduler；等待后恢复才查询 receiver 环境中的启动 scheduler。
- 示例收敛为暂停/单步、线程池分支、取消和普通 sender 四个短程序；同步构建、调试、安装及使用说明。
- 测试和状态模型迁移当前 API、阶段通知与 parent 责任归还语义，并覆盖默认及可选独立分支接口。
- stdexec 默认依赖更新到 `a23aa7abc8796edf158ff67afaf732a6dad3e2a5`，修复多线程完成/取消竞争；配置拒绝已知有缺陷的旧实现，测试同步新的 stop-source 返回值语义。
- 测试源码按运行时、设计契约、编译、打包和仓库检查分目录。
- 示例分为 `examples/cli/` 和 `examples/common/`；安装示例的 CMake 模板归入 `cmake/`。
- 文档按使用、设计和测试分组；示例入口归入首页，开发与发布说明归入贡献指南。
- stdexec 依赖解析与兼容性检查共用一个模块，测试 hook 生成共用一个脚本。
- Windows 构建、调试示例和发布验证统一使用 `scripts/msvc-cmake.cmd`。
- 归档校验和由 CPack 原生生成，随 ZIP/TGZ 提供 `.sha256` 文件。
- 依赖插桩内容未变化时保留生成头文件时间戳，避免重新配置触发重复编译。
- 作为 CMake 子项目时，维护测试和工具链检查默认关闭。
- 发布检查拒绝未进入发布集合的文档链接、失效章节锚点、强制跟踪的忽略文件及悬空符号链接。

### Fixed

- value 的 completion domain 与启动 scheduler 保持一致，修复普通 Pause sender pipeline 直接接入原生线程池时的 domain 编译诊断。

### Removed

- 图形界面示例及其构建、测试和部署入口；库与命令行示例仅使用 ISO C++23 和 stdexec。
- 已退出维护的旧 API 探针、机器专用测试驱动、无界深链诊断和重复构建脚本。
- 依赖旧本地日志的审查报告、场景计数及证据汇总工具；开发记录保留在 Git 历史。

## [0.1.0] — 待首次发布

### Added

- 基于 stdexec 的 C++23 协作式暂停、整体屏障和单步推进。
- 暂停、单步、线程池分支、取消及普通 sender 命令行示例。
- 实现、设计、依赖契约和生命周期回归。
- 可搬移 CMake 开发包、ZIP/TGZ 和安装/更新/卸载管理。

### Compatibility

- 公共命名空间为 `coop_pause`；公共头文件为 `pause.hpp`，目标为 `pause::pause`。
- 维护基线为 Windows/MSVC 和固定 stdexec 提交，其他宿主或平台需另行验证。
