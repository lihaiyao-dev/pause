# 贡献指南

先阅读[使用指南](docs/user-guide.md)、[设计契约](docs/design.md)和[测试指南](tests/README.md)。
修改公共接口或并发协议前，请在 Issue 或 PR 中说明具体场景、预期行为、生命周期
影响和兼容性。安全问题按 [SECURITY.md](SECURITY.md) 处理。

## 开发环境

当前维护基线为 Windows x64 / MSVC、C++23、Ninja 和 CMake 3.28+。
命令从仓库根执行；一般接入步骤见[首页](README.md#快速开始)。
本机安装路径放在个人编辑器设置或被忽略的 `CMakeUserPresets.json`，不要写入公共配置。
库与命令行示例只使用 ISO C++23 标准库和 stdexec（包括随其分发的 `exec` 扩展）。
构建及维护工具与 C++ 依赖分开：CMake/Ninja 用于构建，Python/Git 用于仓库和发布检查。

### 构建与调试

在 VS x64 Developer PowerShell 中生成 Debug 测试和示例：

```powershell
cmake --preset msvc-debug -DPAUSE_BUILD_EXAMPLES=ON
cmake --build --preset msvc-debug --parallel 2
ctest --preset msvc-debug
```

普通终端可使用 `scripts\msvc-cmake.cmd test all` 自动初始化 MSVC。
提交前运行 `python scripts/check-repository.py` 检查源码和文档链接；
不提交生成物、日志、凭据、个人目录和内部服务地址。
VS Code / Cursor 的扩展推荐、任务和启动入口保存在 `.vscode/`：

1. 安装推荐的 LLDB DAP 扩展，并确保可用的 `lldb-dap` 在 `PATH` 中。
2. 打开调试面板，选择 **Pause: simple workflow (LLDB DAP)** 并按 F5。
3. 前置任务会初始化 MSVC、构建命令行示例，然后在 `main` 停下。

其他入口提供线程池、取消和 sender pipeline；线程池 worker 数由下拉框选择。
F10/F11 控制调试器步进，业务的 SingleStep 控制 Pause 屏障轮次，两者含义不同。
可在 `examples/cli/simple_workflow.cpp` 的业务函数和控制 lambda 中设置断点。

需要指定适配器时，在本机编辑器设置中覆盖 `lldb-dap.executable-path`。
某些 LLDB 发行版需要配套 Python 运行时，按其安装要求配置；不要只用 `--version`
判断完整 DAP 会话可用。MSVC 协程恢复帧及局部变量的显示能力取决于调试器版本。

### 代码提示

[.clice/config.toml](.clice/config.toml) 为 Debug/Release 分别选择
`build/msvc-debug` / `build/msvc-release` 编译数据库。首次使用先完成上述配置步骤。

配置中的数据库路径以 `.clice/config.toml` 所在目录为基准。
保持 `readonly = "off"`，以便独立打开的头文件也能按需解析。
修改配置后执行 **Clice: Restart Language Server**，通过
**Clice: Switch Build Configuration** 切换配置，并用
**Clice: Show Current Compilation Context** 检查选用的命令。

`.clangd` 则默认读取 `build/msvc-debug`。切换编译器或自定义输出目录时，
相应调整个人语言服务器设置，不要复制他人缓存或绝对编译路径。

## 代码与验证

- 保持现有命名和四空格缩进；文件编码、换行规则见 `.editorconfig` 和 `.gitattributes`。
- 公共头文件继续放在根目录，示例辅助设施不加入公共 API。
- 并发、取消、异常和资源修改要覆盖完成通道与释放顺序，避免只验证正常路径。
- 新测试加入现有 CMake/CTest 分组；注明外部依赖、运行前提与预期结果。
- 升级 stdexec 时重新审核接口、插桩位置和测试哈希，不只更新版本或摘要。
- 更新当前文档与 `CHANGELOG.md` 的 Unreleased；历史记录保持其日期与证据语义。

## 许可

除另行明确注明的第三方内容外，提交并纳入本项目的贡献采用
`Apache-2.0 WITH LLVM-exception`，与项目 [LICENSE](LICENSE) 一致。
保留引入内容原有的许可及版权声明，并说明来源。

## 提交与 PR

一次提交聚焦一项行为变更，标题可用 `feat:`、`fix:`、`docs:`、`test:` 或 `build:`。
PR 说明问题、最终行为、验证结果和已知限制。没有运行的检查应明确列出。
发布记录使用已确认可公开的身份；如需匿名，可在本仓库配置中性身份或使用 GitHub
提供的 no-reply 邮箱，避免把内部邮箱写入提交历史。

## 发布

版本以根 `CMakeLists.txt` 的 `project(pause VERSION ...)` 为准。
首次公开版本候选为 0.1.0；发布结果以本次验证的日志和产物为准。

### 1. 发布前检查

1. 核对根目录 [LICENSE](LICENSE) 中的 `Apache-2.0 WITH LLVM-exception` 完整条款，
   并确认开发包包含同一份许可文本和第三方说明。
2. 更新 `CHANGELOG.md`，将已完成的 Unreleased 条目归入目标版本并填写真实发布日期。
3. 在新构建目录或干净 checkout 运行本节及下一节的检查。
4. 审阅 `git status` 和将要提交的文件；构建目录、缓存和个人 presets 不进入版本库。
5. 在 GitHub 仓库启用私密漏洞报告，并按需要为默认分支设置 CI 必须通过。

仓库检查需要 Python 3.10+ 和 Git，无额外 Python 包：

```powershell
python scripts/check-repository.py --history
```

需要额外过滤私有名称时，可以重复指定 `--deny <关键词>`。关键词只从命令行读取，
不要把内部名称写回公共规则文件。该检查覆盖源码、文件名、Markdown 本地链接和
常见凭据/个人目录格式，拒绝仅存在于本机忽略目录的文档链接。
它无法判断所有业务保密语义；文档引用的本地文件必须进入发布集合。

### 2. 本地验证

需要 Windows x64、MSVC、Ninja、CMake 3.28+、Python 和首次获取依赖的网络。
从普通终端运行：

```powershell
scripts\msvc-cmake.cmd test debug
scripts\msvc-cmake.cmd release
```

脚本自动初始化 MSVC，执行实现、设计、工具链和命令行示例测试。
Release 还独立安装默认 stdexec，并验证：

- ZIP/TGZ 包内容及文档完整性；
- 安装、更新、卸载和文件所有权；
- 带空格路径、安装目录搬移及重复 `find_package`；
- 离线消费者、随包示例、缺失或不兼容依赖的拒绝；
- 导出目标不包含生产机器路径。

输出位于 `build/msvc-*` 和 `build/release-validation/`，不会安装到系统前缀。
脚本从主构建缓存读取实际 stdexec 源码位置，也支持源码覆盖。
如果主构建使用已安装包，应把 `PAUSE_STDEXEC_SOURCE` 环境变量设为相同固定提交的
stdexec 源码目录，供独立安装验证使用。离线构建需提前准备上游辅助依赖。

未执行的平台、sanitizer 和压力检查应在发布说明列明。

### 3. CI 与产物

[CI 工作流](.github/workflows/ci.yml) 在推送、PR 和手动触发时运行：

- Ubuntu 只执行仓库检查和不依赖编译器的安装管理回归，不声明 Linux C++ 支持。
- Windows/MSVC 执行 Debug、Release 回归；Release 完成真实包集成并上传开发包。

工作流只申请源码读取权限，不创建 Release、不推送提交，也不自动公开发布。
Actions 依赖固定到提交 SHA，由 Dependabot 提出更新；升级时核对
[checkout 发布记录](https://github.com/actions/checkout/releases)与
[upload-artifact 发布记录](https://github.com/actions/upload-artifact/releases)。

成功的 Release 验证会在新的 `build/release-validation/run-*/archives/` 中生成：

```text
pause-<version>.zip
pause-<version>.zip.sha256
pause-<version>.tar.gz
pause-<version>.tar.gz.sha256
```

包内不包含 stdexec。CPack 为每个归档生成 `.sha256` 文件，记录摘要及相对文件名，
用于完整性核验，不是来源签名。仅发布本次成功运行生成的归档和摘要。

### 4. 发布与回退

审阅并提交待发布源码，确认远端 CI 成功，令 tag `v<version>` 与 CMake 版本一致。
在 GitHub 创建 Release 草稿，附上已验证归档、摘要、变更说明、依赖提交、支持范围
和已知限制，审阅后再发布。仓库尚无远程地址时，先配置最终公开仓库地址。

源码归档建议在已审阅提交上使用 `git archive` 或 GitHub 自动生成的源码包，
不要压缩整个本地工作目录。许可证与维护文档应随源码提交。

已安装用户回退时，使用保留的旧开发包执行 `ACTION=update` 并重新构建消费者，
操作和中断恢复步骤见[打包说明](docs/packaging.md)。不要通过改写已发布 tag 静默替换产物。
