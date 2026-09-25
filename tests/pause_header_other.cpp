/* 测试项目：pause_header_compile 的第二翻译单元。
 * 先包含 pause.hpp 再声明/包含 POSIX ::pause，核对 include 顺序兼容和头文件 ODR。
 * 创建 root 并 fork 两个子分支，要求 live_branch_count=2；返回 0 为通过。
 * 与 pause_header_compile.cpp 共同构建和运行；本文件没有独立 main。
 */
#include "pause.hpp"

// Also check the opposite include/declaration order. No function definition or
// call is needed: this regression concerns global name lookup, not POSIX I/O.
#if defined(_WIN32)
extern "C" int pause(void);
#else
#include <unistd.h>
#endif

// This translation unit deliberately emits the same inline public definitions
// as pause_header_compile.cpp, so linking catches header ODR violations.
int pause_header_other()
{
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous, []() noexcept {});
    auto root = source.make_root_branch();
    if (!root) return 1;
    auto children = root->fork_and_block_self<2>();
    if (!children || source.snapshot().live_branch_count != 2) return 2;
    return 0;
}
