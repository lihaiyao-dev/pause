# 许可证与第三方依赖

Pause 自有代码及文档采用 `Apache-2.0 WITH LLVM-exception`，完整条款见
[LICENSE](LICENSE)。该许可包含 Apache License 2.0 和完整 LLVM 例外文本，来源为
[LLVM 官方许可文件](https://llvm.org/LICENSE.txt)。

本文件列出主要依赖及其来源，不替代上游许可证。依赖的许可与版权声明应按所使用
的实际版本保留；项目身份去敏不删除第三方权利信息。

| 组件 | 用途 | 来源及许可文件 |
| --- | --- | --- |
| NVIDIA stdexec | 库的 sender/receiver、task 和调度依赖 | [固定源码](https://github.com/NVIDIA/stdexec/tree/a23aa7abc8796edf158ff67afaf732a6dad3e2a5)，见该提交的 `LICENSE.txt` |
| RAPIDS CMake / ICM | stdexec 的配置期辅助依赖 | 由固定 stdexec 源码管理，许可见相应下载源码 |

Pause 库和命令行示例使用 ISO C++23，唯一的第三方 C++ 依赖是 stdexec；
RAPIDS CMake / ICM 仅为上游的配置期工具，不是 Pause 的 C++ 链接依赖。
Pause 开发包不捆绑 stdexec 或其二进制文件。自动获取依赖只填充本地构建目录。
重新分发第三方源码/二进制时，应随实际产物保留相应许可材料。
