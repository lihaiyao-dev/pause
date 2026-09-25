/* 测试项目：root/main.cpp
 * 历史根目录头文件包含探针, 包含 pause.hpp 并定义空 main.
 * 入口: 手动以仓库根为 include 路径并提供 stdexec 做 C++23 编译; 无 CLI 参数.
 * 预期: 编译链接成功且退出 0; 没有运行期 Pause 行为覆盖.
 * 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
 */
#include "pause.hpp"

int main() {}