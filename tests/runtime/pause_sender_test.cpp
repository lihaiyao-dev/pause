#define PAUSE_ENABLE_INDEPENDENT_BRANCH 1
/* 测试项目：pause_sender_test
 * 性质: 真实 Pause sender 协议、调度次数、取消路径与 operation 生命周期测试.
 * 测试项目:
 * - 编译期检查 C++26 sender/operation_state 协议、完成签名和环境相关的亲和完成属性.
 * - public connect 复制 receiver 抛异常: 不完成、不调度、不改变快照或通知, 随后可重试成功.
 * - Open 内联通过与 Paused 恢复: 覆盖空/非空 stop token、两种恢复模式和完成时自销毁;
 *   Open 不调度, 恢复恰好调度一次并保留原 stop token, 入队本身不代表完成.
 * - 预取消及等待中取消: 覆盖 Open、控制暂停和 SingleStep; 返回 stopped 并撤销参与责任.
 * - 恢复后入队再取消: 由真实 run_loop 检查 stop, 完成 stopped 且计数归零.
 * - start 通知尾部仍在执行时恢复/取消并自销毁 operation: 不重复完成或访问已销毁对象.
 * - 完成回调重入另一个暂停点: 两次内联 value, 无额外调度.
 * - task 单独运行/置于 when_all: 比较 Open、Resume、Stop、QueuedStop 的业务继续情况,
 *   原始调度与宿主亲和补跳次数, 验证最终完成和继续执行回到宿主线程.
 * - 同一 Source 的两个 run_loop: 各分支恢复到各自资源, 每个资源仅增加一次恢复调度.
 * 运行: ctest --test-dir <build> -R '^pause_sender_test$' --output-on-failure.
 * CLI: --iterations N 控制 15 个线程场景 (默认 100), --ordinary-iterations N 控制
 *      28 个普通场景 (默认跟随 iterations); 次数范围 1..1000000.
 * 边界: 使用固定 stdexec 的真实 run_loop 加透明计数适配器; 通知阻塞仅用于测试故障窗口.
 */
#include "../support/pause_test_notifications.hpp"
#include "pause_sender_test_support.hpp"

#include <stdexcept>

namespace {
using namespace pause_sender_test;
int host_schedule_calls = 0;

using Sender = decltype(std::declval<coop_pause::PauseBranch&>().suspend_point());
using Env = decltype(std::declval<Receiver>().get_env());

// Check the C++26 protocol directly: the pinned stdexec also accepts legacy
// completion-signature aliases and operation states without an opt-in tag.
using RawSender = coop_pause::detail::RawSuspendSender;
using RawSignatures = stdexec::completion_signatures<
    stdexec::set_value_t(coop_pause::detail::InlinePass),
    stdexec::set_value_t(coop_pause::detail::ResumedPass),
    stdexec::set_stopped_t()>;
static_assert(std::same_as<decltype(RawSender::get_completion_signatures<RawSender>()),
                           RawSignatures>);
static_assert(std::same_as<decltype(RawSender::get_completion_signatures<RawSender, Env>()),
                           RawSignatures>);
static_assert(std::same_as<stdexec::completion_signatures_of_t<RawSender>, RawSignatures>);
static_assert(std::same_as<stdexec::completion_signatures_of_t<const RawSender&, Env>,
                           RawSignatures>);

struct RawReceiver : Receiver {
    void set_value(coop_pause::detail::InlinePass) noexcept {}
    void set_value(coop_pause::detail::ResumedPass) noexcept {}
};
using RawOperation = stdexec::connect_result_t<RawSender, RawReceiver>;
static_assert(std::same_as<typename RawOperation::operation_state_concept,
                           stdexec::operation_state_tag>);
static_assert(stdexec::operation_state<RawOperation>);

static_assert(exec::get_completion_behavior<stdexec::set_value_t, Sender, Env>()
    == (exec::completion_behavior::inline_completion
        | exec::completion_behavior::asynchronous_affine));
static_assert(stdexec::sender_in<Sender, Env>);
static_assert(exec::get_completion_behavior<stdexec::set_value_t, Sender>()
              == exec::completion_behavior::unknown);
static_assert(exec::get_completion_behavior<stdexec::set_value_t, Sender, stdexec::env<>>()
              == exec::completion_behavior::unknown);

struct ThrowCopyReceiver : Receiver {
    bool* armed;
    ThrowCopyReceiver(ReceiverState* target, bool* flag) noexcept : Receiver{target}, armed(flag) {}
    ThrowCopyReceiver(const ThrowCopyReceiver& other) : Receiver(other), armed(other.armed) {
        if (*armed) throw std::runtime_error("receiver copy before start");
    }
    ThrowCopyReceiver(ThrowCopyReceiver&&) noexcept = default;
};
static_assert(stdexec::receiver<ThrowCopyReceiver>);

void public_connect_failure()
{
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    int notifications = 0;
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous, pause_test::notifications([&]() noexcept { ++notifications; }));
    auto root = source.make_root_branch(); CHECK(root);
    ReceiverState control{&scheduler, stop.get_token()};
    bool armed = false;
    ThrowCopyReceiver receiver{&control, &armed};
    const auto before = source.snapshot();
    const auto prior_notifications = notifications;
    armed = true;
    bool caught = false;
    try {
        // receiver requires noexcept move, but an lvalue receiver copy may
        // throw while creating the outer operation, before start is called.
        auto operation = stdexec::connect(root->suspend_point(), receiver);
        (void)operation;
    } catch (const std::runtime_error&) { caught = true; }
    CHECK(caught && control.result == Completion::None);
    const auto after = source.snapshot();
    CHECK(after.revision == before.revision && after.phase == before.phase);
    CHECK(after.live_branch_count == before.live_branch_count);
    CHECK(after.waiting_branch_count == 0 && notifications == prior_notifications);
    CHECK(scheduler.calls == 0 && scheduler.starts == 0);
    armed = false;
    auto operation = stdexec::connect(root->suspend_point(), receiver);
    run_in_context(scheduler, [&] { stdexec::start(operation); });
    CHECK(control.result == Completion::Value);
}

void raw_route(bool closed, bool cancel, bool empty_token, bool self_destroy,
               coop_pause::AdvanceMode mode = coop_pause::AdvanceMode::SingleStep)
{
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous);
    auto root = source.make_root_branch(); CHECK(root);
    if (closed) CHECK(source.request_pause());
    ReceiverState control{&scheduler, empty_token ? stdexec::inplace_stop_token{} : stop.get_token()};
    scheduler.expected_token = control.token;
    using Operation = stdexec::connect_result_t<Sender, Receiver>;
    auto* operation = new Operation(stdexec::connect(root->suspend_point(), Receiver{&control}));
    control.operation = operation;
    if (self_destroy) control.on_completion = [](ReceiverState* local) noexcept {
        delete static_cast<Operation*>(local->operation); local->operation = nullptr;
    };
    run_in_context(scheduler, [&] {
        control.start_active = true;
        stdexec::start(*operation);
        control.start_active = false;
        if (!closed) {
            CHECK(control.result == Completion::Value);
            CHECK(control.completed_in_start);
            CHECK(scheduler.calls == 0 && scheduler.starts == 0);
            // Open's completed callback must have been unregistered already.
            CHECK(stop.request_stop());
        } else {
            CHECK(control.result == Completion::None);
            auto snap = source.snapshot();
            CHECK(snap.phase == coop_pause::GatePhase::Paused && snap.waiting_branch_count == 1);
            CHECK(source.resume(*snap.paused_epoch, mode));
            CHECK(scheduler.calls == 1 && scheduler.starts == 1);
            CHECK(control.result == Completion::None); // Queue entry is not completion.
            CHECK(source.snapshot().waiting_branch_count == 0);
            if (cancel) CHECK(stop.request_stop());
        }
    });
    CHECK(control.result == (cancel ? Completion::Stopped : Completion::Value));
    CHECK(control.completion_thread == scheduler.thread);
    CHECK(scheduler.calls == (closed ? 1 : 0));
    CHECK(scheduler.starts == (closed ? 1 : 0));
    if (closed) CHECK(scheduler.original_tokens == 1);
    if (cancel) {
        const auto snap = source.snapshot();
        CHECK(snap.live_branch_count == 0);
        CHECK(snap.phase == (mode == coop_pause::AdvanceMode::SingleStep
            ? coop_pause::GatePhase::Paused : coop_pause::GatePhase::Open));
        CHECK(snap.waiting_branch_count == 0);
    }
    if (!self_destroy) delete operation;
}

void callback_stop(bool prestopped, bool self_destroy, int gate = 2)
{
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    coop_pause::PauseSource source(gate == 2 ? coop_pause::AdvanceMode::SingleStep
                                      : coop_pause::AdvanceMode::Continuous);
    auto root = source.make_root_branch(); CHECK(root);
    if (gate == 1) CHECK(source.request_pause());
    ReceiverState control{&scheduler, stop.get_token()};
    using Operation = stdexec::connect_result_t<Sender, Receiver>;
    auto* operation = new Operation(stdexec::connect(root->suspend_point(), Receiver{&control}));
    control.operation = operation;
    if (self_destroy) control.on_completion = [](ReceiverState* local) noexcept {
        delete static_cast<Operation*>(local->operation); local->operation = nullptr;
    };
    if (prestopped) CHECK(stop.request_stop());
    run_in_context(scheduler, [&] {
        stdexec::start(*operation);
        if (!prestopped) {
            CHECK(source.snapshot().waiting_branch_count == 1);
            CHECK(stop.request_stop());
        }
    });
    CHECK(control.result == Completion::Stopped);
    CHECK(scheduler.calls == 0 && scheduler.starts == 0);
    CHECK(source.snapshot().live_branch_count == 0);
    if (!self_destroy) delete operation;
}

void waiting_start_tail(bool cancel)
{
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    Gate notification;
    std::atomic<bool> armed{false};
    // Test-only fault-injection gate. Production notifiers must not block;
    // here an independent coordinator releases the tail to verify lifetime.
    coop_pause::PauseSource source(coop_pause::AdvanceMode::SingleStep, pause_test::notifications([&]() noexcept {
        if (armed.exchange(false)) notification.hold();
    }));
    auto root = source.make_root_branch(); CHECK(root);
    ReceiverState control{&scheduler, stop.get_token()};
    using Operation = stdexec::connect_result_t<Sender, Receiver>;
    auto* operation = new Operation(stdexec::connect(root->suspend_point(), Receiver{&control}));
    control.operation = operation;
    control.on_completion = [](ReceiverState* local) noexcept {
        delete static_cast<Operation*>(local->operation); local->operation = nullptr;
    };
    armed = true;
    std::jthread starter([&] {
        run_in_context(scheduler, [&] { stdexec::start(*operation); });
    });
    notification.entered.wait();
    CHECK(source.snapshot().waiting_branch_count == 1);
    if (cancel) {
        CHECK(stop.request_stop());
        CHECK(control.result == Completion::Stopped && control.operation == nullptr);
        CHECK(source.snapshot().live_branch_count == 0);
    } else {
        CHECK(source.resume(*source.snapshot().paused_epoch, coop_pause::AdvanceMode::Continuous));
        CHECK(scheduler.calls == 1 && scheduler.starts == 1);
        CHECK(control.result == Completion::None);
    }
    notification.release.count_down();
    starter.join();
    CHECK(control.operation == nullptr);
    CHECK(control.result == (cancel ? Completion::Stopped : Completion::Value));
}

void completion_reenters()
{
    SchedulerState scheduler;
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous);
    auto root = source.make_root_branch(); CHECK(root);
    ReceiverState control{&scheduler, {}};
    int values = 0;
    auto chain = stdexec::let_value(root->suspend_point(), [&] {
        ++values; return stdexec::then(root->suspend_point(), [&] { ++values; });
    });
    auto operation = stdexec::connect(std::move(chain), Receiver{&control});
    run_in_context(scheduler, [&] { stdexec::start(operation); });
    CHECK(values == 2 && control.result == Completion::Value);
    CHECK(scheduler.calls == 0 && scheduler.starts == 0);
}

enum class Route { Open, Resume, Stop, QueuedStop };
struct TaskControl {
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    ReceiverState receiver{&scheduler, stop.get_token()};
    coop_pause::PauseBranch* branch = nullptr;
    stdexec::inplace_stop_token task_token;
    std::atomic<bool> continued{false};
    std::thread::id continuation_thread;
    int calls_before = 0, calls_after = 0;
};
stdexec::task<void> pause_task(TaskControl& control) {
    control.task_token = co_await stdexec::read_env(stdexec::get_stop_token);
    CHECK(control.task_token.stop_possible());
    control.calls_before = control.scheduler.calls;
    co_await control.branch->suspend_point();
    control.calls_after = control.scheduler.calls;
    control.continuation_thread = std::this_thread::get_id();
    control.continued = true;
}
stdexec::task<void> empty_task() { co_return; }
template<class S>
void run_task(TaskControl& control, S sender) {
    auto operation = stdexec::connect(
        stdexec::starts_on(CountingScheduler{&control.scheduler}, std::move(sender)),
        Receiver{&control.receiver});
    stdexec::start(operation);
    control.scheduler.thread = std::this_thread::get_id();
    control.scheduler.loop.run();
}
void task_route(Route route, bool when_all,
                coop_pause::AdvanceMode mode = coop_pause::AdvanceMode::SingleStep)
{
    TaskControl control;
    Gate startup, original_stopped;
    const bool open = route == Route::Open;
    coop_pause::PauseSource source(open ? coop_pause::AdvanceMode::Continuous : coop_pause::AdvanceMode::SingleStep);
    auto root = source.make_root_branch(); CHECK(root);
    control.branch = &*root;
    if (!open) control.scheduler.after_first_value = &startup;
    if (route == Route::QueuedStop) control.scheduler.after_stopped = &original_stopped;
    std::jthread worker([&] {
        if (when_all) run_task(control, stdexec::when_all(pause_task(control), stdexec::just()));
        else run_task(control, pause_task(control));
    });
    if (!open) {
        startup.entered.wait();
        CHECK((control.task_token != control.stop.get_token()) == when_all);
        CHECK(source.snapshot().waiting_branch_count == 1);
        if (route == Route::Resume || route == Route::QueuedStop) {
            CHECK(source.resume(*source.snapshot().paused_epoch, mode));
            CHECK(control.scheduler.calls == host_schedule_calls + 1 && control.scheduler.starts == 2);
            CHECK(source.snapshot().live_branch_count == 1);
            if (route == Route::QueuedStop) {
                CHECK(control.stop.request_stop());
                startup.release.count_down();
                original_stopped.entered.wait();
            }
        } else CHECK(control.stop.request_stop());
        if (route == Route::Stop || route == Route::QueuedStop) {
            CHECK(source.snapshot().live_branch_count == 0);
            CHECK(source.snapshot().phase == (route == Route::QueuedStop
                  && mode == coop_pause::AdvanceMode::Continuous
                ? coop_pause::GatePhase::Open : coop_pause::GatePhase::Paused));
            CHECK(source.snapshot().waiting_branch_count == 0);
            CHECK(control.receiver.result == Completion::None && !control.continued);
        }
        if (route == Route::QueuedStop) original_stopped.release.count_down();
        else startup.release.count_down();
    }
    worker.join();
    const bool value = route == Route::Open || route == Route::Resume;
    const int resume = route == Route::Resume || route == Route::QueuedStop ? 1 : 0;
    const int host = value ? 0 : 1;
    CHECK(control.calls_before == host_schedule_calls);
    CHECK(control.scheduler.calls == host_schedule_calls + resume + host);
    CHECK(control.scheduler.starts == 1 + resume + host);
    CHECK(control.receiver.result == (value ? Completion::Value : Completion::Stopped));
    CHECK(control.continued == value);
    CHECK(control.receiver.completion_thread == control.scheduler.thread);
    if (value) {
        CHECK(control.calls_after - control.calls_before == resume);
        CHECK(control.continuation_thread == control.scheduler.thread);
    }
    CHECK(control.scheduler.stops == (route == Route::QueuedStop ? 1 : 0));
}

void two_resources()
{
    TaskControl first, second;
    Gate a, b;
    first.scheduler.after_first_value = &a;
    second.scheduler.after_first_value = &b;
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous);
    auto root = source.make_root_branch(); CHECK(root);
    auto other = source.make_branch(); CHECK(other);
    first.branch = &*root; second.branch = &*other;
    CHECK(source.request_pause());
    std::jthread one([&] { run_task(first, pause_task(first)); });
    std::jthread two([&] { run_task(second, pause_task(second)); });
    a.entered.wait(); b.entered.wait();
    CHECK(source.snapshot().waiting_branch_count == 2);
    CHECK(source.resume(*source.snapshot().paused_epoch, coop_pause::AdvanceMode::Continuous));
    a.release.count_down(); b.release.count_down();
    one.join(); two.join();
    CHECK(first.continued && second.continued);
    CHECK(first.continuation_thread == first.scheduler.thread);
    CHECK(second.continuation_thread == second.scheduler.thread);
    CHECK(first.scheduler.thread != second.scheduler.thread);
    CHECK(first.scheduler.calls == host_schedule_calls + 1 && second.scheduler.calls == host_schedule_calls + 1);
}
} // namespace

int main(int argc, char** argv) {
    int count = 100, ordinary_count = 0;
    CHECK(argc % 2 == 1);
    for (int index = 1; index < argc; index += 2) {
        const std::string_view option(argv[index]), value(argv[index + 1]);
        int parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        CHECK(error == std::errc{} && end == value.data() + value.size() && parsed > 0 && parsed <= 1000000);
        if (option == "--iterations") count = parsed;
        else { CHECK(option == "--ordinary-iterations"); ordinary_count = parsed; }
    }
    if (ordinary_count == 0) ordinary_count = count;
    TaskControl baseline;
    run_task(baseline, empty_task());
    CHECK(baseline.scheduler.starts == 1);
    host_schedule_calls = baseline.scheduler.calls;
    for (int i = 0; i != ordinary_count; ++i) {
        for (bool destroy : {false, true}) {
            raw_route(false, false, false, destroy);
            raw_route(false, false, true, destroy);
            raw_route(true, false, false, destroy);
            raw_route(true, false, true, destroy);
            raw_route(true, true, false, destroy);
            raw_route(true, false, false, destroy, coop_pause::AdvanceMode::Continuous);
            raw_route(true, false, true, destroy, coop_pause::AdvanceMode::Continuous);
            raw_route(true, true, false, destroy, coop_pause::AdvanceMode::Continuous);
            callback_stop(false, destroy);
            callback_stop(true, destroy);
            callback_stop(true, destroy, 0); // Open: pre-cancel wins before InlinePass.
            callback_stop(true, destroy, 1); // ControlRequest Collecting.
            callback_stop(false, destroy, 1);
        }
        completion_reenters();
        public_connect_failure();
    }
    std::printf("PASS: sender ordinary 28 cases x %d iterations\n", ordinary_count);
    std::fflush(stdout);
    for (int i = 0; i != count; ++i) {
        waiting_start_tail(false); waiting_start_tail(true);
        for (bool join : {false, true})
            for (auto route : {Route::Open, Route::Resume, Route::Stop, Route::QueuedStop})
                task_route(route, join);
        for (bool join : {false, true})
            for (auto route : {Route::Resume, Route::QueuedStop})
                task_route(route, join, coop_pause::AdvanceMode::Continuous);
        two_resources();
    }
    std::printf("PASS: sender threaded 15 cases x %d iterations\n", count);
    if (ordinary_count == count)
        std::printf("PASS: 43 real Pause sender/scheduler/lifetime cases x %d iterations\n", count);
    else
        std::printf("PASS: %d real Pause sender/scheduler/lifetime cases\n", ordinary_count * 28 + count * 15);
}
