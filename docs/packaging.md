# Pause 开发包（0.1.0）

Pause 是 C++23 头文件库，不生成 `.lib`/`.dll`。开发包包含 `pause.hpp`、
`pause::pause` 导出目标、配置期兼容性探针和安装管理脚本，不包含 stdexec。

安装后从 `<prefix>/share/pause/README.md` 查阅库介绍与使用指南，包括 API 速查、
暂停/单步语义、取消和生命周期要求及故障排查。同目录的 `examples/` 提供简单示例、
辅助头文件和独立 CMake 工程，可直接使用已安装的 `pause::pause` 构建。
文档和示例纳入包清单，随更新替换、随卸载移除；请在安装目录外构建或修改示例。

## 构建 ZIP / TGZ

在已有 C++23 编译环境的终端执行（Windows 使用 VS x64 Developer PowerShell）：

```powershell
cmake -S . -B build/package -G Ninja -DCMAKE_BUILD_TYPE=Release -DPAUSE_BUILD_TESTS=OFF -DPAUSE_BUILD_DESIGN_TESTS=OFF -DPAUSE_BUILD_TOOLCHAIN_SMOKE_TEST=OFF
cmake --build build/package
cpack --config build/package/CPackConfig.cmake -B build/packages
```

生成 `pause-0.1.0.zip` 和 `pause-0.1.0.tar.gz`。两个包内容相同，只有 Pause
组件，无构建目录绝对路径；无需把 stdexec 源码随包分发。打包阶段仍编译依赖兼容性探针。
`PAUSE_INSTALL=OFF` 可关闭安装/打包规则；作为子项目时默认关闭。
可用相对的 `CMAKE_INSTALL_INCLUDEDIR` / `CMAKE_INSTALL_DATADIR` 定制布局。

## 安装、更新、卸载

解压后假设目录为 `D:/downloads/pause-0.1.0`，目标前缀为 `D:/sdk/pause`：

```powershell
cmake -DACTION=install -DPREFIX=D:/sdk/pause -P D:/downloads/pause-0.1.0/manage-pause.cmake
# 解压新版本到另一个目录，使用新版本的脚本执行 update；也支持重装同版。
cmake -DACTION=update -DPREFIX=D:/sdk/pause -P D:/downloads/pause-0.1.0/manage-pause.cmake
cmake -DACTION=uninstall -DPREFIX=D:/sdk/pause -P D:/sdk/pause/manage-pause.cmake
```

以上操作只需 CMake 3.25+，无需编译器、源码树或网络。路径含空格时给整个 `-D...`
参数加引号。前缀必须是绝对路径，并与解压目录互不包含。Linux 使用相同脚本和
例如 `/home/user/sdk/pause` 的前缀。解压目录本身也可直接作为 `CMAKE_PREFIX_PATH`。

每个前缀只管理一个 Pause 版本。清单记录文件相对路径与 SHA256；更新/卸载先验证
已安装文件，发现缺失、修改、路径越界、符号链接或新文件与其他库冲突即停止。
不会以“强制覆盖”方式丢弃本地修改。更新删除旧清单中不再存在的文件；卸载保留其他
库的文件、空目录及 `.pause.lock`。重复卸载可从原解压包执行，结果为成功。
SHA256 用于完整性检测，不是来源签名；仅使用可信发布包和自己控制的安装前缀。

安装管理有十秒锁超时，先备份旧文件并暂存新文件，再执行替换。检测到复制/删除
失败会尝试回滚；这不是文件系统事务，断电、进程强杀以及正在编译的读者不在原子性
保证内。更新期间暂停使用该前缀的构建。若留下 `.pause-transaction`，后续操作会拒绝：

1. 若没有 `commit-started.txt`，安装内容尚未开始变更；检查后删除该事务目录。
2. 若已有该文件，先备份现场。按 `affected.txt`（分号分隔的相对路径）删除本次涉及的
   文件，然后将 `backup/` 的内容恢复到前缀；`backup-complete.txt` 是旧文件清单。
   确认恢复完成后再删除事务目录。不要递归删除整个安装前缀。

回退版本使用保留的旧发布包执行 `ACTION=update`，然后重新配置、编译所有消费者。
不会更新或卸载独立安装的 stdexec。也可以使用标准源码安装：

```powershell
cmake --install build/package --prefix D:/sdk/pause --component Pause
```

标准 `cmake --install` 不执行上述冲突/更新保护，建议只用于空前缀或打包暂存；安装完成
也会生成清单，可由管理脚本接管。后续版本发布需修改根 `project(VERSION ...)`。
0.x 阶段使用 `SameMinorVersion`：同一 minor 内接受满足请求的更高 patch，不承诺跨 minor 兼容。

## 消费者与依赖解析

```cmake
cmake_minimum_required(VERSION 3.25)
project(consumer LANGUAGES CXX)
find_package(pause 0.1 CONFIG REQUIRED)
add_executable(app main.cpp)
target_link_libraries(app PRIVATE pause::pause)
```

`main.cpp` 可直接 `#include <pause.hpp>`。C++23、stdexec 使用要求与 MSVC `/EHsc`
通过目标传递。配置时传入 `-DCMAKE_PREFIX_PATH=D:/sdk/pause`；多个前缀用分号分隔
并整体加引号。头文件包可搬移；搬移后更新前缀并重新配置消费者。

源码构建和已安装包使用同一依赖策略：

1. 优先复用已有 `STDEXEC::stdexec` 目标，否则 `find_package(stdexec CONFIG QUIET)`。
2. 找到后编译检查实际接口；失败给出警告并终止配置，**不下载其他版本替换**。
3. 未找到时用 FetchContent 下载固定提交
   `f0e8ae6fdc6c188389b146bb854b80d399724b04`，同样执行接口检查。

自动下载仅填充当前构建目录，不向系统安装依赖。CMake 包查找遵循 `stdexec_DIR`、
`CMAKE_PREFIX_PATH` 和标准搜索规则，不扫描所有磁盘上的裸头文件。破损的已发现配置也
不会被自动替换。没有标准包配置的旧安装需指定/安装正确的 stdexec 包。

可指定 `PAUSE_STDEXEC_GIT_TAG` 或 `FETCHCONTENT_SOURCE_DIR_STDEXEC`；这些覆盖仅在
未发现已有依赖时生效。离线使用推荐独立安装 stdexec，并设置
`PAUSE_FETCH_STDEXEC=OFF`，未找到时立即报错。使用本地源码覆盖时上游仍可能需要
RAPIDS/ICM 等构建辅助依赖。自动下载固定提交要求 **CMake 3.28+**（上游要求），
且首次配置需要 Git、网络和支持相应 C++23 特性的编译器。

独立安装 stdexec 的示例（`<stdexec-source>` 为兼容提交）：

```powershell
cmake -S <stdexec-source> -B build/stdexec -G Ninja -DCMAKE_BUILD_TYPE=Release -DSTDEXEC_INSTALL=ON -DSTDEXEC_BUILD_TESTS=OFF -DSTDEXEC_BUILD_EXAMPLES=OFF
cmake --install build/stdexec --prefix D:/sdk/stdexec
cmake -S <consumer-source> -B build/consumer "-DCMAKE_PREFIX_PATH=D:/sdk/pause;D:/sdk/stdexec"
cmake --build build/consumer
```

stdexec 的 `0.11.0` 不能区分所需 `.affine()` 接口是否存在，因此不使用数字版本作为
充分条件。检查 `.affine()` 定制、`completion_behavior`、真实 Pause/default task 接口；
失败详情位于消费者构建目录的 `pause-stdexec-check.log`。检查只编译静态库、不执行
目标程序，适用于交叉编译。通过检查不等于完整并发/生命周期兼容认证。项目内部插桩
测试仍要求默认提交的头文件哈希；改用其他兼容版本时关闭实现测试和设计测试。

## 采用的上游惯例

- [NVIDIA/stdexec 固定提交的 CMakeLists.txt](https://github.com/NVIDIA/stdexec/blob/f0e8ae6fdc6c188389b146bb854b80d399724b04/CMakeLists.txt)：独立安装、命名空间目标与导出使用要求。
- [CMake 导入/导出指南](https://cmake.org/cmake/help/latest/guide/importing-exporting/index.html)：`install(EXPORT)` 与可搬移目标。
- [CMakePackageConfigHelpers](https://cmake.org/cmake/help/latest/module/CMakePackageConfigHelpers.html)：可搬移 Config 与架构无关版本文件。
- [CPack Archive](https://cmake.org/cmake/help/latest/cpack_gen/archive.html)：ZIP/TGZ 开发包。

标准 `find_dependency(... REQUIRED)` 会在依赖缺失时直接退出，不满足本项目获准的
自动下载要求，因此使用共享解析函数执行“查找 → 必要时下载 → 接口检查”。安装管理
是本项目对可搬移开发包的补充脚本，不声称是 CMake 自带的卸载/事务功能。
