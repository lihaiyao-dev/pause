/* 测试项目：公开 API 的类型约束和 receiver 环境编译检查。
 * 静态断言要求 Source/Branch 的工厂、resume、fork 只接受可变左值，并限制赋值操作。
 * PAUSE_ENVIRONMENT_CASE=0 必须编译成功；1 缺少 scheduler，2 错误 stop_token，
 * 3 缺少 stop_token，4 非法 scheduler 均须在 connect 时编译失败（包括 Open 路径）。
 * 由 check_pause_environment.cmake 驱动，CTest：pause_environment_compile。
 * 这是编译期正反例，无 main；失败判据见驱动，不执行 operation 或运行时压力。
 */
#include "pause.hpp"

#include <stop_token>
#include <type_traits>
#include <utility>

template<class T>
concept CanMakeRoot = requires(T&& value) { std::forward<T>(value).make_root_branch(); };
template<class T>
concept CanMakeIndependent = requires(T&& value) { std::forward<T>(value).make_branch(); };
template<class T>
concept CanResume = requires(T&& value) {
    std::forward<T>(value).resume(coop_pause::PauseEpoch{1}, coop_pause::AdvanceMode::Continuous);
};
template<class T>
concept CanForkFixed = requires(T&& value) { std::forward<T>(value).template fork_and_block_self<1>(); };
template<class T>
concept CanForkDynamic = requires(T&& value) { std::forward<T>(value).fork_and_block_self(1); };
using Source = coop_pause::PauseSource;
using Branch = coop_pause::PauseBranch;
static_assert(CanMakeRoot<Source&> && !CanMakeRoot<const Source&> && !CanMakeRoot<Source>);
static_assert(CanMakeIndependent<Source&> && !CanMakeIndependent<const Source&> && !CanMakeIndependent<Source>);
static_assert(CanResume<Source&> && !CanResume<const Source&> && !CanResume<Source>);
static_assert(CanForkFixed<Branch&> && !CanForkFixed<const Branch&> && !CanForkFixed<Branch>);
static_assert(CanForkDynamic<Branch&> && !CanForkDynamic<const Branch&> && !CanForkDynamic<Branch>);
static_assert(!std::is_copy_assignable_v<Source> && !std::is_move_assignable_v<Source>);
static_assert(!std::is_copy_assignable_v<Branch>);

// PAUSE_ENVIRONMENT_CASE=0 is the positive control. Cases 1 through 4 must be
// rejected at connect even if the Pause gate would take the Open path.
#ifndef PAUSE_ENVIRONMENT_CASE
#define PAUSE_ENVIRONMENT_CASE 0
#endif

struct CompileReceiver {
    using receiver_concept = stdexec::receiver_tag;
    stdexec::run_loop* loop;
    stdexec::inplace_stop_source* stop;
    auto get_env() const noexcept
    {
#if PAUSE_ENVIRONMENT_CASE == 1
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, stop->get_token()}};
#elif PAUSE_ENVIRONMENT_CASE == 2
        return stdexec::env{
            stdexec::prop{stdexec::get_start_scheduler, loop->get_scheduler()},
            stdexec::prop{stdexec::get_stop_token, std::stop_token{}}};
#elif PAUSE_ENVIRONMENT_CASE == 3
        return stdexec::env{
            stdexec::prop{stdexec::get_start_scheduler, loop->get_scheduler()}};
#elif PAUSE_ENVIRONMENT_CASE == 4
        return stdexec::env{
            stdexec::prop{stdexec::get_start_scheduler, 42},
            stdexec::prop{stdexec::get_stop_token, stop->get_token()}};
#else
        return stdexec::env{
            stdexec::prop{stdexec::get_start_scheduler, loop->get_scheduler()},
            stdexec::prop{stdexec::get_stop_token, stop->get_token()}};
#endif
    }
    void set_value() noexcept {}
    void set_stopped() noexcept {}
    template<class Error> void set_error(Error&&) noexcept {}
};

void pause_environment_compile(coop_pause::PauseBranch& branch,
                               stdexec::run_loop& loop,
                               stdexec::inplace_stop_source& stop)
{
    auto operation = stdexec::connect(branch.suspend_point(), CompileReceiver{&loop, &stop});
    (void)operation;
}
