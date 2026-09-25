/* 测试项目：release-extreme-tsan/posix_pause_shim.hpp
 * 历史 Linux/TSan 辅助夹具, 包含 unistd.h 时重命名 POSIX ::pause 避免与旧 namespace pause 冲突.
 * 入口: 历史 clang++ 命令通过 -include 强制包含, 没有 main.
 * 预期: 后续 include 不产生冲突; 测试不得调用重命名的 POSIX 函数, 当前 coop_pause API 不依赖此夹具.
 * 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
 */
// Test-only: no test calls the renamed POSIX function. Its header guard prevents
// a later stdlib include from redeclaring ::pause beside namespace pause.
#define pause pause_posix_declaration_for_tsan_only
#include <unistd.h>
#undef pause
