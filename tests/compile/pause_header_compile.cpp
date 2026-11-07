/* 测试项目：公共头与 POSIX 全局 pause 名称共存，以及多翻译单元链接。
 * 本文件先声明/包含 ::pause，再包含 pause.hpp；另一个翻译单元检查相反顺序。
 * 创建 root 后要求 live_branch_count=1，再调用 pause_header_other 核对 fork。
 * CTest：pause_header_compile；两个源文件须一起链接，返回 0 为通过。
 * 只验证头文件、命名空间及基本构造，不调用 POSIX pause，也不做线程压力。
 */
// Check a pre-existing global POSIX name without a declaration-renaming shim.
#if defined(_WIN32)
extern "C" int pause(void);
#else
#include <unistd.h>
#endif

#include "pause.hpp"

int pause_header_other();

int main()
{
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous);
    auto root = source.make_root_branch();
    if (!root || source.snapshot().live_branch_count != 1) return 1;
    return pause_header_other();
}
