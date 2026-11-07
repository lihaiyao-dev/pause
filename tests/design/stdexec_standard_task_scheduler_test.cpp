/* 测试项目：stdexec_standard_task_scheduler_test
 * 目的: 实测新 stdexec::task 的调度、取消 token 和公开 affine 定制协议。
 * 测试项:
 * 1. 先测宿主基线, 区分 Debug 额外构造 sender 的 schedule 调用与实际 start。
 * 2. Open/Resumed/Stopped/Error x 直接属性/affine 定制/外包 then x 有无 when_all。
 * 3. Resumed 排队后停止, 核对原始跳转接受取消及 when_all 的 token 替换和传播。
 * 4. 两个独立 scheduler 资源; 异线程原始 stopped/error 返回各自宿主资源。
 * 5. callback 停止与排队停止均先撤销内部责任, 再等待额外宿主 affinity 跳转。
 * 预期: 逐路核对 schedule/start 次数、线程、token、完成通道和业务是否继续;
 *       直接 affine 的 Open 无额外跳转、Resumed 一次, stopped/error 另有一次真实跳转;
 *       外包 then 隐藏 affine 成员后恢复默认额外调度, stopped 后不继续业务。
 * 入口: 配置 PAUSE_BUILD_DESIGN_TESTS=ON, CTest 名称 stdexec_standard_task_scheduler_test。
 *       CLI --iterations N, N 为正整数, 默认 100, 每轮 35 个协议场景。
 * 边界: ProtocolSender 是依赖可行性探针, 不含 pause.hpp, 无真实屏障、分支或回调所有权。
 *       此探针为观察传播而转发 error, 不实现真实 Pause 的内部错误 terminate 策略;
 *       所测属性/定制行为仅代表固定依赖版本, 不能直接折算为 Pause 场景通过数。
 */
#include <exec/completion_behavior.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <latch>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

// Pinned stdexec::task scheduler/environment protocol probes. This is NOT a
// Pause implementation: no barrier accounting, callback ownership, or fork tree.
namespace {

[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

struct CompletionGate {
    std::latch entered{1};
    std::latch release{1};

    void hold() noexcept
    {
        entered.count_down();
        release.wait();
    }
};

struct SchedulerState {
    stdexec::run_loop loop;
    std::atomic<int> calls{0};
    std::atomic<int> starts{0};
    std::atomic<int> values{0};
    std::atomic<int> stopped{0};
    std::thread::id thread;
    CompletionGate* after_first_value = nullptr;
    CompletionGate* after_stopped = nullptr;
};

struct CountingScheduleSender;
struct CountingScheduler {
    using scheduler_concept = stdexec::scheduler_tag;
    SchedulerState* state;
    bool operator==(const CountingScheduler&) const noexcept = default;
    auto schedule() const noexcept -> CountingScheduleSender;
};

struct CountingScheduleSender {
    using sender_concept = stdexec::sender_tag;
    using BaseSender = stdexec::schedule_result_t<stdexec::run_loop::scheduler>;
    SchedulerState* state;

    struct Attrs {
        SchedulerState* state;
        template<class Tag>
            requires (std::is_same_v<Tag, stdexec::set_value_t>
                      || std::is_same_v<Tag, stdexec::set_stopped_t>)
        auto query(stdexec::get_completion_scheduler_t<Tag>) const noexcept
        {
            return CountingScheduler{state};
        }
        template<class Tag>
        static constexpr auto query(exec::get_completion_behavior_t<Tag>) noexcept
        {
            return exec::completion_behavior::asynchronous;
        }
    };
    auto get_env() const noexcept { return Attrs{state}; }

    template<class, class Env>
    static consteval auto get_completion_signatures() noexcept
    {
        return stdexec::completion_signatures_of_t<BaseSender, Env>{};
    }

    template<class R>
    struct Receiver {
        using receiver_concept = stdexec::receiver_tag;
        SchedulerState* state;
        R receiver;
        auto get_env() const noexcept { return stdexec::get_env(receiver); }
        void set_value() noexcept
        {
            auto* gate = state->after_first_value;
            const bool first = ++state->values == 1;
            stdexec::set_value(std::move(receiver));
            // Test-only loop gate. Completion may have destroyed this receiver;
            // only the separately owned gate is used after the terminal CPO.
            if (first && gate) gate->hold();
        }
        void set_stopped() noexcept
        {
            auto* gate = state->after_stopped;
            ++state->stopped;
            stdexec::set_stopped(std::move(receiver));
            if (gate) gate->hold();
        }
    };
    template<class R>
    struct Operation {
        SchedulerState* state;
        stdexec::connect_result_t<BaseSender, Receiver<R>> inner;
        Operation(SchedulerState* scheduler, R receiver)
            : state(scheduler), inner(stdexec::connect(
                  stdexec::schedule(scheduler->loop.get_scheduler()),
                  Receiver<R>{scheduler, std::move(receiver)}))
        {}
        void start() & noexcept
        {
            ++state->starts;
            stdexec::start(inner);
        }
    };
    template<class R>
    auto connect(R receiver) const { return Operation<R>{state, std::move(receiver)}; }
};

auto CountingScheduler::schedule() const noexcept -> CountingScheduleSender
{
    ++state->calls;
    return {state};
}
static_assert(stdexec::scheduler<CountingScheduler>);

enum class Route { Open, Resumed, Stopped, Error };
enum class Completion { None, Value, Stopped, Error };

// An ordering sentinel only, not Pause's live/pending/parent accounting.
struct ResponsibilityProbe {
    std::atomic<int> live{1};
    std::atomic<int> withdrawals{0};

    void withdraw() noexcept
    {
        if (live.exchange(0) != 0) ++withdrawals;
    }
};

struct Control {
    SchedulerState scheduler;
    stdexec::inplace_stop_source source;
    Route route = Route::Open;
    bool cancel_after_queue = false;
    bool complete_in_start = true;
    bool expect_when_all_token = false;
    bool continued = false;
    bool then_ran = false;
    bool start_active = false;
    bool then_inline = false;
    int calls_before_await = -1;
    int calls_after_await = -1;
    std::atomic<int> pause_schedules{0};
    std::atomic<int> adapted_schedules{0};
    ResponsibilityProbe* responsibility = nullptr;
    stdexec::inplace_stop_token task_token;
    std::thread::id raw_completion_thread;
    std::thread::id task_continuation_thread;
    std::thread::id root_completion_thread;
    Completion result = Completion::None;
    std::exception_ptr expected_error = std::make_exception_ptr(std::runtime_error("probe error"));
    std::latch published{1};
    void* operation = nullptr;
    void (*complete)(void*) noexcept = nullptr;
};

template<bool Adapt>
struct ProtocolSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(), stdexec::set_stopped_t(), stdexec::set_error_t(std::exception_ptr)>;
    Control* control;

    struct Attrs {
        // Exactly the design's current value-only property. It is deliberately
        // silent about callback-thread stopped/error execution resources.
        template<class Env>
            requires requires(const Env& env) { stdexec::get_start_scheduler(env); }
        static constexpr auto query(exec::get_completion_behavior_t<stdexec::set_value_t>,
                                    const Env&) noexcept
        {
            return exec::completion_behavior::inline_completion
                 | exec::completion_behavior::asynchronous_affine;
        }
    };
    auto get_env() const noexcept { return Attrs{}; }

    template<class R>
    struct Operation {
        Control* control;
        R receiver;
        using Scheduler = std::decay_t<decltype(stdexec::get_start_scheduler(
            stdexec::get_env(std::declval<const R&>())))>;
        Scheduler scheduler;

        struct ScheduleReceiver {
            using receiver_concept = stdexec::receiver_tag;
            Operation* operation;
            auto get_env() const noexcept -> stdexec::env_of_t<R>
            {
                return stdexec::get_env(operation->receiver);
            }
            void set_value() noexcept { operation->value(); }
            void set_stopped() noexcept { operation->stopped(); }
            template<class Error>
            void set_error(Error&& error) noexcept
            {
                stdexec::set_error(std::move(operation->receiver), static_cast<Error&&>(error));
            }
        };
        using Scheduled = stdexec::connect_result_t<stdexec::schedule_result_t<Scheduler>,
                                                     ScheduleReceiver>;
        std::unique_ptr<Scheduled> scheduled;

        Operation(Control* state, R target)
            : control(state), receiver(std::move(target)),
              scheduler(stdexec::get_start_scheduler(stdexec::get_env(receiver)))
        {}
        void value() noexcept
        {
            control->raw_completion_thread = std::this_thread::get_id();
            stdexec::set_value(std::move(receiver));
        }
        void stopped() noexcept
        {
            // Models the inner Pause stopped wrapper's placement: withdraw
            // before forwarding to the host's additional affine adaptation.
            if (control->responsibility) control->responsibility->withdraw();
            control->raw_completion_thread = std::this_thread::get_id();
            stdexec::set_stopped(std::move(receiver));
        }
        void finish() noexcept
        {
            auto* local = control;
            if (local->route == Route::Resumed) {
                ++local->pause_schedules;
                scheduled.reset(new Scheduled(stdexec::connect(
                    stdexec::schedule(scheduler), ScheduleReceiver{this})));
                // For the queued-stop case this runs inside the loop's start
                // invocation; that loop cannot execute the queue until return.
                stdexec::start(*scheduled);
                if (local->cancel_after_queue) CHECK(local->source.request_stop());
                return;
            }
            if (local->route == Route::Open) value();
            else if (local->route == Route::Stopped) stopped();
            else {
                local->raw_completion_thread = std::this_thread::get_id();
                stdexec::set_error(std::move(receiver), local->expected_error);
            }
        }
        void start() & noexcept
        {
            auto* local = control;
            auto token = stdexec::get_stop_token(stdexec::get_env(receiver));
            static_assert(std::is_same_v<decltype(token), stdexec::inplace_stop_token>);
            CHECK(token == local->task_token);
            local->operation = this;
            local->complete = [](void* pointer) noexcept { static_cast<Operation*>(pointer)->finish(); };
            local->start_active = true;
            local->published.count_down();
            if (local->complete_in_start) finish();
            // Only the externally owned control is touched after completion.
            local->start_active = false;
        }
    };
    template<class R>
    auto connect(R receiver) const { return Operation<R>{control, std::move(receiver)}; }

    // Public customization feasibility probe, not a proposed production change.
    // It preserves value routing, and really restores the starting execution
    // resource for stopped/error. Only that extra affinity hop is unstoppable.
    auto affine() const requires Adapt
    {
        auto stopped = stdexec::let_stopped(ProtocolSender<false>{control}, [state = control] {
            return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
                [state](auto scheduler) {
                    ++state->adapted_schedules;
                    return stdexec::let_value(stdexec::unstoppable(stdexec::schedule(scheduler)),
                        [] { return stdexec::just_stopped(); });
                });
        });
        return stdexec::let_error(std::move(stopped), [state = control](std::exception_ptr error) {
            return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
                [state, error](auto scheduler) {
                    ++state->adapted_schedules;
                    return stdexec::let_value(stdexec::unstoppable(stdexec::schedule(scheduler)),
                        [error] { return stdexec::just_error(error); });
                });
        });
    }
};

struct RootReceiver {
    using receiver_concept = stdexec::receiver_tag;
    Control* control;
    auto get_env() const noexcept
    {
        return stdexec::env{
            stdexec::prop{stdexec::get_stop_token, control->source.get_token()},
            stdexec::prop{stdexec::get_start_scheduler, CountingScheduler{&control->scheduler}}};
    }
    void complete(Completion result) noexcept
    {
        CHECK(control->result == Completion::None);
        control->result = result;
        control->root_completion_thread = std::this_thread::get_id();
        control->scheduler.loop.finish();
    }
    void set_value() noexcept { complete(Completion::Value); }
    void set_stopped() noexcept { complete(Completion::Stopped); }
    void set_error(std::exception_ptr error) noexcept
    {
        // task rethrows/captures errors; exception_ptr identity is not its
        // propagation contract (notably on the MSVC exception runtime).
        bool expected = false;
        try { std::rethrow_exception(error); }
        catch (const std::runtime_error& caught) { expected = std::string(caught.what()) == "probe error"; }
        catch (...) {}
        CHECK(expected);
        complete(Completion::Error);
    }
};

using ProbeEnv = decltype(std::declval<RootReceiver>().get_env());
static_assert(exec::get_completion_behavior<stdexec::set_value_t, ProtocolSender<false>, ProbeEnv>()
              == (exec::completion_behavior::inline_completion
                  | exec::completion_behavior::asynchronous_affine));
static_assert(exec::get_completion_behavior<stdexec::set_stopped_t, ProtocolSender<false>, ProbeEnv>()
              == exec::completion_behavior::unknown);
static_assert(exec::get_completion_behavior<stdexec::set_error_t, ProtocolSender<false>, ProbeEnv>()
              == exec::completion_behavior::unknown);

template<bool Adapt, bool Then>
stdexec::task<void> protocol_task(Control& control)
{
    control.task_token = co_await stdexec::read_env(stdexec::get_stop_token);
    CHECK(control.task_token.stop_possible());
    CHECK((control.task_token != control.source.get_token()) == control.expect_when_all_token);
    auto current = co_await stdexec::read_env(stdexec::get_start_scheduler);
    static_assert(std::is_same_v<decltype(current), stdexec::task_scheduler>);
    CHECK(current == stdexec::task_scheduler{CountingScheduler{&control.scheduler}});
    control.calls_before_await = control.scheduler.calls.load();
    if constexpr (Then) {
        co_await stdexec::then(ProtocolSender<Adapt>{&control}, [&control] {
            control.then_ran = true;
            control.then_inline = control.start_active;
        });
    } else {
        co_await ProtocolSender<Adapt>{&control};
    }
    control.calls_after_await = control.scheduler.calls.load();
    control.task_continuation_thread = std::this_thread::get_id();
    control.continued = true;
}

stdexec::task<void> empty_task() { co_return; }

template<class Sender>
void run(Control& control, Sender sender)
{
    auto operation = stdexec::connect(
        stdexec::starts_on(CountingScheduler{&control.scheduler}, std::move(sender)),
        RootReceiver{&control});
    stdexec::start(operation);
    control.scheduler.thread = std::this_thread::get_id();
    control.scheduler.loop.run();
}

int baseline()
{
    Control control;
    run(control, empty_task());
    CHECK(control.result == Completion::Value);
    std::printf("baseline: schedule_calls=%d starts=%d values=%d stopped=%d\n",
                control.scheduler.calls.load(), control.scheduler.starts.load(),
                control.scheduler.values.load(), control.scheduler.stopped.load());
    // The pinned get_completion_scheduler query constructs one extra sender
    // inside STDEXEC_ASSERT in Debug; it never starts that diagnostic sender.
    // Keep the measured host baseline separate from the awaited protocol.
#ifdef NDEBUG
    constexpr int expected_calls = 1;
#else
    constexpr int expected_calls = 2;
#endif
    CHECK(control.scheduler.calls == expected_calls && control.scheduler.starts == 1);
    return control.scheduler.calls.load();
}

template<bool Adapt, bool Then>
void test_route(Route route, bool cancel, bool when_all, int host_baseline)
{
    Control control;
    control.route = route;
    control.cancel_after_queue = cancel;
    control.expect_when_all_token = when_all;
    if (when_all)
        run(control, stdexec::when_all(protocol_task<Adapt, Then>(control), stdexec::just()));
    else
        run(control, protocol_task<Adapt, Then>(control));

    const bool value = route == Route::Open || (route == Route::Resumed && !cancel);
    const bool error = route == Route::Error;
    CHECK(control.result == (value ? Completion::Value : error ? Completion::Error : Completion::Stopped));
    CHECK(control.continued == value);
    CHECK(control.root_completion_thread == control.scheduler.thread);
    if (value) CHECK(control.task_continuation_thread == control.scheduler.thread);
    if constexpr (Then) CHECK(control.then_ran == value);
    if constexpr (Then) {
        if (route == Route::Open) CHECK(control.then_inline);
    }
    const int pause = route == Route::Resumed ? 1 : 0;
    const int extra = Adapt && !Then && value ? 0 : 1;
    CHECK(control.pause_schedules == pause);
    CHECK(control.scheduler.calls == host_baseline + pause + extra);
    CHECK(control.scheduler.starts == 1 + pause + extra);
    CHECK(control.adapted_schedules == (Adapt && !Then && !value ? 1 : 0));
    CHECK(control.calls_before_await == host_baseline);
    if (value) CHECK(control.calls_after_await - control.calls_before_await == pause + extra);
    if (cancel) {
        CHECK(control.task_token.stop_requested());
        CHECK(control.scheduler.stopped == 1); // Original Pause hop accepts cancellation.
    }
}

void test_two_resources(int host_baseline)
{
    Control first, second;
    first.route = second.route = Route::Resumed;
    first.complete_in_start = second.complete_in_start = false;
    std::jthread a([&] { run(first, protocol_task<true, false>(first)); });
    std::jthread b([&] { run(second, protocol_task<true, false>(second)); });
    first.published.wait(); second.published.wait();
    // Raw release comes from the independent coordinator, then each protocol
    // sender explicitly schedules on its own saved task_scheduler resource.
    first.complete(first.operation); second.complete(second.operation);
    a.join(); b.join();
    CHECK(first.scheduler.thread != second.scheduler.thread);
    CHECK(first.task_continuation_thread == first.scheduler.thread);
    CHECK(second.task_continuation_thread == second.scheduler.thread);
    CHECK(first.scheduler.calls == host_baseline + 1 && second.scheduler.calls == host_baseline + 1);
    CHECK(first.scheduler.starts == 2 && second.scheduler.starts == 2);
    CHECK(first.pause_schedules == 1 && second.pause_schedules == 1);
    CHECK(first.result == Completion::Value && second.result == Completion::Value);
}

void test_cross_thread_non_value(Route route, int host_baseline)
{
    Control control;
    control.route = route;
    control.complete_in_start = false;
    std::jthread runner([&] { run(control, protocol_task<true, false>(control)); });
    control.published.wait();
    const auto coordinator = std::this_thread::get_id();
    control.complete(control.operation);
    runner.join();
    CHECK(control.raw_completion_thread == coordinator);
    CHECK(control.raw_completion_thread != control.scheduler.thread);
    CHECK(control.root_completion_thread == control.scheduler.thread);
    CHECK(control.result == (route == Route::Stopped ? Completion::Stopped : Completion::Error));
    CHECK(!control.continued);
    CHECK(control.pause_schedules == 0 && control.adapted_schedules == 1);
    CHECK(control.scheduler.calls == host_baseline + 1 && control.scheduler.starts == 2);
}

void test_withdraw_before_host_stop(bool resumed, int host_baseline)
{
    CompletionGate initial_host, original_schedule_stopped;
    ResponsibilityProbe responsibility;
    Control control;
    control.route = resumed ? Route::Resumed : Route::Stopped;
    control.complete_in_start = false;
    control.cancel_after_queue = resumed;
    control.responsibility = &responsibility;
    control.scheduler.after_first_value = &initial_host;
    if (resumed) control.scheduler.after_stopped = &original_schedule_stopped;

    std::jthread runner([&] { run(control, protocol_task<true, false>(control)); });
    control.published.wait();
    initial_host.entered.wait();
    CHECK(responsibility.live == 1 && responsibility.withdrawals == 0);
    CHECK(control.result == Completion::None);

    const auto coordinator = std::this_thread::get_id();
    if (resumed) {
        // The loop is held at its initial completion tail. Queue the original
        // cancellable resume and request stop before it can execute.
        control.complete(control.operation);
        CHECK(control.task_token.stop_requested());
        CHECK(responsibility.live == 1 && responsibility.withdrawals == 0);
        CHECK(control.scheduler.stopped == 0 && control.adapted_schedules == 0);
        initial_host.release.count_down();
        // The original schedule accepts stop, performs inner withdrawal, and
        // queues the host affinity hop. Its completion tail then holds the loop.
        original_schedule_stopped.entered.wait();
    } else {
        int callback_calls = 0;
        auto on_stop = [&]() noexcept {
            ++callback_calls;
            control.complete(control.operation);
        };
        stdexec::inplace_stop_callback<decltype(on_stop)> callback{control.task_token, on_stop};
        CHECK(control.source.request_stop());
        CHECK(callback_calls == 1);
        // request_stop/callback returned on this coordinator thread while the
        // host loop is still held. This is a callback delivery protocol probe,
        // not Pause's actual callback claim/reset state machine.
    }

    // Both paths have accepted stopped and retired the inner responsibility,
    // but the extra host queue entry has not executed or completed the task.
    CHECK(responsibility.live == 0 && responsibility.withdrawals == 1);
    CHECK(control.adapted_schedules == 1);
    CHECK(control.result == Completion::None && !control.continued);
    CHECK(control.scheduler.values == 1); // Only host startup completed value.
    CHECK(control.scheduler.stopped == (resumed ? 1 : 0));
    CHECK(control.pause_schedules == (resumed ? 1 : 0));
    CHECK(control.scheduler.starts == (resumed ? 3 : 2));

    if (resumed) original_schedule_stopped.release.count_down();
    else initial_host.release.count_down();
    runner.join();
    CHECK(control.result == Completion::Stopped && !control.continued);
    CHECK(control.root_completion_thread == control.scheduler.thread);
    CHECK(control.raw_completion_thread == (resumed ? control.scheduler.thread : coordinator));
    CHECK(responsibility.live == 0 && responsibility.withdrawals == 1);
    CHECK(control.scheduler.calls == host_baseline + (resumed ? 2 : 1));
    CHECK(control.scheduler.values == 2); // Startup + unstoppable host affinity.
    CHECK(control.scheduler.stopped == (resumed ? 1 : 0));
}

} // namespace

int main(int argc, char** argv)
{
    int iterations = 100;
    if (argc == 3 && std::string(argv[1]) == "--iterations") iterations = std::stoi(argv[2]);
    else CHECK(argc == 1);
    CHECK(iterations > 0);
    const int host = baseline();
    for (int i = 0; i < iterations; ++i) {
        for (Route route : {Route::Open, Route::Resumed, Route::Stopped, Route::Error}) {
            for (bool join : {false, true}) {
                test_route<false, false>(route, false, join, host);
                test_route<true, false>(route, false, join, host);
                test_route<true, true>(route, false, join, host);
            }
        }
        for (bool join : {false, true}) {
            test_route<false, false>(Route::Resumed, true, join, host);
            test_route<true, false>(Route::Resumed, true, join, host);
            test_route<true, true>(Route::Resumed, true, join, host);
        }
        test_two_resources(host);
        test_cross_thread_non_value(Route::Stopped, host);
        test_cross_thread_non_value(Route::Error, host);
        test_withdraw_before_host_stop(false, host);
        test_withdraw_before_host_stop(true, host);
    }
    std::printf("PASS: iterations=%d cases_per_iteration=35 host_schedule_calls=%d host_starts=1\n",
                iterations, host);
    std::puts("design value-only attrs: Open=1 extra schedule, Resumed=2 schedules excluding host");
    std::puts("public affine adaptation: Open=0, Resumed=1; stopped/error gain one real affinity hop");
    std::puts("outer then hides affine member: Open=1, Resumed=2; when_all replaces token, propagates stop");
    std::puts("queued stop accepted by original resumed hop; two saved scheduler resources verified");
    std::puts("cross-thread raw stopped/error restored to task resource by public affine adaptation");
    std::puts("callback/queued stop: responsibility withdrawn before held host hop; task then stopped, never value");
    std::puts("protocol probe only: no real Pause barrier, callback ownership, fork, or termination policy");
}
