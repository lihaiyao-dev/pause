/* 测试项目：配置期 stdexec 接口兼容性探针。
 * 检查 affine 实际选择 sender 的成员定制（无 starting scheduler 的对照环境），
 * 并实例化真实 Pause suspend_point 与默认 stdexec::task 宿主的 co_await 接口。
 * cmake/PauseDependency.cmake 每次配置用 try_compile 生成静态库；应编译成功。
 * 无 main、无运行；不证明 ABI 布局、线程安全或所有 stdexec 版本兼容。
 */
#include <pause.hpp>
#include <exec/static_thread_pool.hpp>

namespace {

struct AffineProbeSender {
    using sender_concept = stdexec::sender_tag;

    template<class, class... Env>
    static consteval auto get_completion_signatures()
    {
        return stdexec::completion_signatures<stdexec::set_value_t()>{};
    }

    template<class Receiver>
    auto connect(Receiver receiver) &&
    {
        return stdexec::connect(stdexec::just(), std::move(receiver));
    }

    auto affine() && { return stdexec::just(); }
};

struct AffineProbeReceiver {
    using receiver_concept = stdexec::receiver_tag;

    auto get_env() const noexcept { return stdexec::env<>{}; }
    void set_value() noexcept {}
};

// With no declared completion behavior and no starting scheduler, the default
// affine adapter cannot connect. The library must actually select .affine().
// Both the original and customized senders have the same completion signature.
[[maybe_unused]] void check_affine_customization()
{
    auto operation = stdexec::connect(stdexec::affine(AffineProbeSender{}),
                                     AffineProbeReceiver{});
    stdexec::start(operation);
}

// Instantiate the real public sender and its default task host as well as the
// focused customization probe. Neither function is executed by CMake.
[[maybe_unused]] stdexec::task<void> check_pause_task(coop_pause::PauseBranch branch)
{
    co_await branch.suspend_point();
}

template<stdexec::scheduler Scheduler>
void check_native_task(Scheduler scheduler, coop_pause::PauseBranch branch)
{
    static_assert(std::constructible_from<stdexec::task_scheduler, Scheduler>);
    (void)stdexec::sync_wait(stdexec::starts_on(scheduler, check_pause_task(std::move(branch))));
}

[[maybe_unused]] void check_native_schedulers(coop_pause::PauseBranch first, coop_pause::PauseBranch second)
{
    exec::static_thread_pool pool{1};
    check_native_task(pool.get_scheduler(), std::move(first));
    stdexec::run_loop loop;
    check_native_task(loop.get_scheduler(), std::move(second));
}

} // namespace
