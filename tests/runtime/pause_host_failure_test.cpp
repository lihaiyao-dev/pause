/* 测试项目：pause_host_failure_test
 * 目的: 区分宿主线程/控制器故障与 Pause 自身资源错误, 验证异常清理顺序.
 * 测试项:
 * - controller-creation-failure: 工作线程已运行, 工作协程尚未启动时, 在模拟
 *   控制器线程构造边界抛出 resource_unavailable_try_again 的 system_error.
 * - controller-business-failure: 使用 examples/common/example_support.hpp 的真实
 *   run_scope, 控制器在暂停屏障处抛出业务异常, 触发工作流取消与退出清理.
 * 关键预期: 第一项停止并 join 已启动工作线程, 未启动协程及其分支只销毁一次;
 * 第二项只在控制器/工作线程和 request_stop 尾部退出后重抛原异常, 工作流不越过
 * 暂停点; 两项最终责任清零, 快照 phase/revision/epoch 和通知数符合断言.
 * 入口: PAUSE_BUILD_TESTS=ON 时注册同名 CTest.
 *   ctest --test-dir <build> -C <config> -R "^pause_host_failure_test$" --output-on-failure
 *   pause_host_failure_test --iterations 2
 * 参数: iterations 为 1..1000000, 默认两项分别执行 2 次.
 * 边界: 使用真实 Pause, 第一项仅在等效构造失败的展开点注入异常, 不声称实际
 * 耗尽操作系统线程; 本程序验证宿主清理, 不替代 Pause 分配故障矩阵.
 */
#include "../support/pause_test_notifications.hpp"
#include "pause_sender_test_support.hpp"
#include "../../examples/common/example_support.hpp"

#include <system_error>

// Host cleanup is separate from Pause's own allocation/error policy. Inject at
// the thread-construction boundary; never exhaust the operating system's threads.
namespace {
using namespace pause_sender_test;
using namespace pause_examples;

struct Lifetime {
    std::atomic<int> started{0}, passed{0}, destroyed{0};
};

struct FrameTicket {
    Lifetime* lifetime;
    explicit FrameTicket(Lifetime& current) noexcept : lifetime(&current) {}
    FrameTicket(const FrameTicket&) = delete;
    FrameTicket(FrameTicket&& other) noexcept
        : lifetime(std::exchange(other.lifetime, nullptr)) {}
    ~FrameTicket() { if (lifetime) ++lifetime->destroyed; }
};

stdexec::task<void> workflow(PauseBranch branch, FrameTicket ticket)
{
    ++ticket.lifetime->started;
    co_await branch.suspend_point();
    ++ticket.lifetime->passed;
}

struct WorkerState {
    std::atomic<int> ran{0}, exited{0}, joined{0};
};

void fail_controller_creation(stdexec::task<void> pending, WorkerState& state)
{
    stdexec::run_loop loop;
    std::jthread worker([&]() noexcept {
        loop.run();
        ++state.exited;
    });
    exec::scope_guard finish_worker{[&]() noexcept {
        loop.finish();
        worker.join();
        CHECK(state.exited == 1);
        ++state.joined;
    }};
    // Completing a queued sender proves run() has actually begun. The workflow
    // parameter still owns its never-started coroutine and branch throughout.
    CHECK(stdexec::sync_wait(stdexec::schedule(loop.get_scheduler())
                            | stdexec::then([&]() noexcept { ++state.ran; })));
    CHECK(state.ran == 1 && state.exited == 0);
    (void)pending;
    // The same stack-unwinding point as a throwing jthread controller ctor.
    throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
}

void controller_creation_failure()
{
    ChangeSignal changed;
    Lifetime lifetime;
    WorkerState worker;
    PauseSource source{AdvanceMode::Continuous, pause_test::notifications([&]() noexcept { changed.signal(); })};
    bool caught = false;
    try {
        fail_controller_creation(
            workflow(take(source.make_root_branch()), FrameTicket{lifetime}), worker);
    } catch (const std::system_error& error) {
        CHECK(error.code() == std::make_error_code(std::errc::resource_unavailable_try_again));
        caught = true;
    }
    CHECK(caught && worker.ran == 1 && worker.exited == 1 && worker.joined == 1);
    CHECK(lifetime.started == 0 && lifetime.passed == 0 && lifetime.destroyed == 1);
    require_drained(source);
    CHECK(source.snapshot().phase == GatePhase::Open);
    CHECK(source.snapshot().revision == 2 && changed.sequence == 0);
}

struct ControllerFailure {};

void controller_business_failure()
{
    ChangeSignal changed;
    Lifetime lifetime;
    std::atomic<int> notifications{0}, controller_failed{0};
    PauseSource source{AdvanceMode::Continuous, pause_test::notifications([&]() noexcept {
        ++notifications;
        changed.signal();
    })};
    auto root = take(source.make_root_branch());
    CHECK(source.request_pause());
    bool caught = false;
    try {
        // Exercise the existing example host itself, including its controller
        // exception capture, workflow cancellation, wakeup and both join guards.
        (void)run_scope(changed, workflow(std::move(root), FrameTicket{lifetime}),
            [&](std::stop_token quit, auto&) {
                auto paused = wait_for_barrier(quit, changed, source);
                CHECK(paused && paused->phase == GatePhase::Paused);
                CHECK(paused->waiting_branch_count == 1 && paused->live_branch_count == 1);
                ++controller_failed;
                throw ControllerFailure{};
            });
        CHECK(false);
    } catch (const ControllerFailure&) {
        caught = true;
    }
    // run_scope rethrows only after the controller's request_stop tail and the
    // worker are joined. Source and its notifier captures remain alive here.
    CHECK(caught && controller_failed == 1);
    CHECK(lifetime.started == 1 && lifetime.passed == 0 && lifetime.destroyed == 1);
    require_drained(source);
    const auto final = source.snapshot();
    CHECK(final.phase == GatePhase::Paused && final.paused_epoch == coop_pause::PauseEpoch{3});
    CHECK(final.revision == 5 && notifications == 2);
}

template<class Function>
void run_case(const char* id, int count, Function function)
{
    for (int iteration = 0; iteration != count; ++iteration) function();
    std::printf("PASS case=%s complete_scopes=%d\n", id, count);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    int count = 2;
    CHECK(argc == 1 || (argc == 3 && std::string_view(argv[1]) == "--iterations"));
    if (argc == 3) {
        const std::string_view value(argv[2]);
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
        CHECK(error == std::errc{} && end == value.data() + value.size() && count > 0 && count <= 1000000);
    }
    run_case("host.controller-creation-failure", count, controller_creation_failure);
    run_case("host.controller-business-failure", count, controller_business_failure);
    std::printf("Pause host failure: %d complete scopes, 2 threaded x %d\n", count * 2, count);
}
