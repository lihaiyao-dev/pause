/* 测试项目：pause_contract_death_test
 * 目的: 在独立子进程中验证契约外误用触发 require() 的 terminate 诊断.
 * 测试项: 空 notifier; 对已移动分支调用 suspend_point; 子分支仍存活时释放
 * 父分支; 子分支仍存活时父分支重新进入; 同一分支启动第二个活动 waiter.
 * 关键预期: 每项只有到达指定误用前置条件后触发 terminate 并退出 77 才通过;
 * 准备期间 terminate 为 78, 未终止返回为 79, 启动失败或任意信号均不得误判通过.
 * 入口: PAUSE_BUILD_TESTS=ON 时注册同名 CTest, 目标强制定义 NDEBUG 以绕开
 * 平台 assert UI 并检查 require 的终止分支.
 *   ctest --test-dir <build> -C <config> -R "^pause_contract_death_test$" --output-on-failure
 *   pause_contract_death_test --iterations 1
 * 参数: iterations 为 1..1000000, 默认每项一个子进程; --child <case> 是
 * 父进程内部使用的单场景入口, 预期非零退出, 不应直接当作普通通过测试运行.
 * 边界: 这些场景属于契约外诊断, 不代表支持这些生命周期/并发用法; 子进程应
 * 在已检查的前置条件处终止, 不以制造悬空存储或数据竞争来验证未定义行为.
 */
#include "pause_sender_test_support.hpp"

#include <exception>
#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

// This target defines NDEBUG to exercise require()'s terminate fallback, not
// a platform's assert UI. These invalid uses are implementation diagnostics,
// never examples of supported lifetime or concurrency. Each child stops at
// the checked precondition, before dangling storage or a data race is created.
#ifndef NDEBUG
#error Build this diagnostic probe with NDEBUG.
#endif

namespace {
using namespace pause_sender_test;
using coop_pause::AdvanceMode;
using coop_pause::PauseSource;
bool armed = false;

[[noreturn]] void child(std::string_view name)
{
    std::set_terminate([] { std::_Exit(armed ? 77 : 78); });
    if (name == "empty-notifier") {
        armed = true;
        PauseSource source(AdvanceMode::Continuous, {});
    } else {
        PauseSource source(AdvanceMode::SingleStep, []() noexcept {});
        auto root = source.make_root_branch(); CHECK(root);
        if (name == "moved-from-suspend") {
            auto owner = std::move(*root);
            armed = true;
            auto sender = root->suspend_point();
            (void)sender;
        } else if (name == "parent-release-with-children") {
            auto children = root->fork_and_block_self<1>(); CHECK(children);
            armed = true;
            root = std::unexpected(coop_pause::PauseError::InvalidBranchState);
        } else {
            SchedulerState scheduler;
            ReceiverState result{&scheduler, {}};
            if (name == "parent-reentry-with-children") {
                auto children = root->fork_and_block_self<1>(); CHECK(children);
                auto operation = stdexec::connect(root->suspend_point(), Receiver{&result});
                run_in_context(scheduler, [&] { armed = true; stdexec::start(operation); });
            } else {
                CHECK(name == "second-active-waiter");
                auto first = stdexec::connect(root->suspend_point(), Receiver{&result});
                auto second = stdexec::connect(root->suspend_point(), Receiver{&result});
                run_in_context(scheduler, [&] {
                    stdexec::start(first);
                    CHECK(source.snapshot().waiting_branch_count == 1);
                    armed = true;
                    stdexec::start(second);
                });
            }
        }
    }
    std::_Exit(79); // Returning, arbitrary signals, or another exit are failures.
}

int launch(const char* executable, const char* name)
{
#if defined(_WIN32)
    const char* arguments[]{executable, "--child", name, nullptr};
    return static_cast<int>(_spawnv(_P_WAIT, executable, arguments));
#else
    const auto pid = fork(); CHECK(pid >= 0);
    if (pid == 0) { execl(executable, executable, "--child", name, nullptr); std::_Exit(80); }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
} // namespace

int main(int argc, char** argv)
{
    constexpr const char* cases[]{"empty-notifier", "moved-from-suspend",
        "parent-release-with-children", "parent-reentry-with-children", "second-active-waiter"};
    if (argc == 3 && std::string_view(argv[1]) == "--child") {
        bool known = false;
        for (const char* name : cases) known |= std::string_view(argv[2]) == name;
        CHECK(known);
        child(argv[2]);
    }
    const int count = argc == 1 ? 1 : iterations(argc, argv);
    for (const char* name : cases) {
        for (int iteration = 0; iteration != count; ++iteration) {
            const int result = launch(argv[0], name);
            if (result != 77)
                std::fprintf(stderr, "diagnostic=%s iteration=%d exit=%d expected=77\n", name, iteration, result);
            CHECK(result == 77);
        }
        std::printf("PASS diagnostic=%s subprocesses=%d expected_exit=77\n", name, count);
        std::fflush(stdout);
    }
}
