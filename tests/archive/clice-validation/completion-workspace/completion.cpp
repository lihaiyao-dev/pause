/* 测试项目：clice-validation/completion-workspace/completion.cpp
 * clice 补全夹具: snapshot/suspend_point 中设置命名 U+00A7 标记, _DEBUG 区分配置.
 * 入口: 同目录 clice.toml 的 debug/release 夹具编译数据库供 clice inspect 读取.
 * 预期: 补全分别含 snapshot/suspend_point; 标记刻意保留, 不是可直接编译源文件.
 * 前提: 支持命名标记的历史 clice 和保留 pause:: API 的历史 Pause 头文件.
 * 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
 */
#include "pause.hpp"
void clice_probe(pause::PauseSource& source, pause::PauseBranch& branch) {
    auto snapshot = source.sn§(source_members)apshot();
    auto sender = branch.su§(branch_members)spend_point();
}
#ifdef _DEBUG
struct CliceDebugMode {};
#else
struct CliceReleaseMode {};
#endif