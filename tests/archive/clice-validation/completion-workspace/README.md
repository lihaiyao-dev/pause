# clice 历史补全夹具

这里保存手写的编辑器测试输入, 不加入 CMake 或 CTest. `simple.cpp` 和
`completion.cpp` 中的 `§` 是 clice 光标标记, 并非有效 C++ 代码. 需由支持
`--annotations` 的历史 clice 去除标记并在对应位置请求补全, 不可直接交给编译器.

| 文件 | 测试项目与预期 |
| --- | --- |
| `simple.cpp` | 普通结构体成员补全应包含 `value`. |
| `completion.cpp` | 命名位置 `source_members` 和 `branch_members` 的补全分别包含 `snapshot` 和 `suspend_point`; `_DEBUG` 分支区分两种配置. |
| `clice.toml` | 默认选择 `debug`, 按配置从 `debug` 或 `release` 子目录加载编译数据库. |
| `debug/compile_commands.json` | 手工从历史 MSVC Debug 命令制作的夹具输入, 保留 `/MDd` 和 Debug 参数. |
| `release/compile_commands.json` | 手工从历史 MSVC Release 命令制作的夹具输入, 保留 `/O2`, `/DNDEBUG` 和 `/MD`. |

两个 JSON 是测试配置输入, 因 JSON 不支持注释, 在此说明其用途. 它们已修正
`completion.cpp` 的输入位置; 编译器路径, `build/msvc-*` 工作目录, 依赖和输出项
保留原值. 它们不是当前项目自动生成的编译数据库, 也不会由本目录自动重新生成.

历史 clice 支持以下入口, 可在本目录按配置读取夹具:

```text
clice inspect code_completion completion.cpp --annotations --configuration debug
clice inspect code_completion completion.cpp --annotations --configuration release
```

运行前需恢复数据库指定的 MSVC/stdexec 依赖与仍提供 `pause::` 命名空间的历史
Pause 头文件. 当前 `coop_pause::` API 不直接兼容这份历史输入. `simple.cpp` 没有
专用数据库条目, 需由宿主提供合适编译参数或相应历史补全环境. 此次只整理夹具和
源码路径, 未运行 clice. 缓存, 日志和真实构建产物仍保留在 `build`.
