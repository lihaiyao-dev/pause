/* 测试项目：scheduler_stop_probe (历史新旧 task scheduler 取消对照)
 * 测试项: 相同 inplace_stop_token 下先将 run_loop、stdexec::task_scheduler、旧 exec::task scheduler
 * 排队再请求停止, 与 inline_scheduler 对照; 另验证新 stdexec::task 显式调度后停止传播。
 * 预期: run_loop 和新 task_scheduler 交付 stopped; 旧 erased scheduler 与 inline 仍交付 value;
 * 新 task 的根完成为 stopped, 调度后的协程业务不继续, 每种观察重复 100 次。
 * 入口: 同目录 build-scheduler-stop-probe.cmd 与 run-scheduler-stop-probe.cmd; 不接受 CLI 参数。
 * 边界: 不含 pause.hpp, 仅固定依赖取消行为的历史证据; 旧 exec::task 是不兼容对照。
 * 不能从 token 已停止推导任意 scheduler 必须停止, 当前回归入口见 tests/stdexec_standard_task_*。
 */
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <type_traits>
#include <utility>

namespace {
[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

enum class Completion { None, Value, Stopped };

struct Result {
    Completion completion = Completion::None;
    bool requested_at_completion = false;
    std::shared_ptr<void> operation;
};

struct Control {
    stdexec::run_loop loop;
    stdexec::inplace_stop_source source;
    Result direct;
    Result task_scheduler;
    Result old_task_scheduler;
    Result inline_scheduler;
    Completion root_completion = Completion::None;
    bool continued = false;
};

struct Receiver {
    using receiver_concept = stdexec::receiver_tag;
    stdexec::inplace_stop_source* source;
    Result* result;

    auto get_env() const noexcept
    {
        return stdexec::prop{stdexec::get_stop_token, source->get_token()};
    }
    void set_value() noexcept { complete(Completion::Value); }
    void set_stopped() noexcept { complete(Completion::Stopped); }
    template<class Error>
    void set_error(Error&&) noexcept { fail("unexpected scheduler error", __LINE__); }
    void complete(Completion completion) noexcept
    {
        CHECK(result->completion == Completion::None);
        result->requested_at_completion = source->get_token().stop_requested();
        result->completion = completion;
    }
};

struct RootReceiver {
    using receiver_concept = stdexec::receiver_tag;
    Control* control;

    auto get_env() const noexcept
    {
        return stdexec::env{
            stdexec::prop{stdexec::get_stop_token, control->source.get_token()},
            stdexec::prop{stdexec::get_start_scheduler, control->loop.get_scheduler()}};
    }
    void set_value() noexcept { complete(Completion::Value); }
    void set_stopped() noexcept { complete(Completion::Stopped); }
    template<class Error>
    void set_error(Error&&) noexcept { fail("unexpected task error", __LINE__); }
    void complete(Completion completion) noexcept
    {
        CHECK(control->root_completion == Completion::None);
        control->root_completion = completion;
        control->loop.finish();
    }
};

template<class Scheduler>
void start_probe(Scheduler scheduler, stdexec::inplace_stop_source& source, Result& result)
{
    Receiver receiver{&source, &result};
    static_assert(std::is_same_v<
        decltype(stdexec::get_stop_token(receiver.get_env())), stdexec::inplace_stop_token>);
    CHECK(stdexec::get_stop_token(receiver.get_env()) == source.get_token());
    using Operation = stdexec::connect_result_t<
        decltype(stdexec::schedule(scheduler)), Receiver>;
    auto operation = std::shared_ptr<Operation>{new Operation(
        stdexec::connect(stdexec::schedule(scheduler), receiver))};
    result.operation = operation;
    stdexec::start(*operation);
}

exec::task<void> enqueue_comparison(Control& control)
{
    auto current = co_await stdexec::read_env(stdexec::get_start_scheduler);
    auto task_token = co_await stdexec::read_env(stdexec::get_stop_token);
    CHECK(task_token == control.source.get_token());
    const auto concrete = control.loop.get_scheduler();

    // This task runs on the loop thread through starts_on. The loop cannot run
    // these three queue entries until this invocation returns. All receivers
    // are given the identical inplace_stop_token from the same source.
    start_probe(concrete, control.source, control.direct);
    start_probe(stdexec::task_scheduler{concrete}, control.source, control.task_scheduler);
    start_probe(current, control.source, control.old_task_scheduler);
    CHECK(control.direct.completion == Completion::None);
    CHECK(control.task_scheduler.completion == Completion::None);
    CHECK(control.old_task_scheduler.completion == Completion::None);
    CHECK(!control.source.request_stop());
    CHECK(task_token.stop_requested());

    // Inline has no queue. Its receiver sees the same requested token, while
    // the scheduler's completion policy still chooses value.
    start_probe(stdexec::inline_scheduler{}, control.source, control.inline_scheduler);
    CHECK(control.inline_scheduler.completion == Completion::Value);
    CHECK(control.inline_scheduler.requested_at_completion);
    co_return;
}

// Only public sender APIs are used. For a run_loop-backed scheduler start()
// returns after queue insertion and before completion; the source is requested
// precisely in that interval, on the same loop thread.
template<class Scheduler>
struct StopAfterQueueSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(), stdexec::set_stopped_t()>;
    Scheduler scheduler;
    stdexec::inplace_stop_source* source;

    template<class R>
    struct Operation {
        stdexec::inplace_stop_source* source;
        stdexec::connect_result_t<decltype(stdexec::schedule(std::declval<Scheduler>())), R> inner;

        Operation(Scheduler scheduler, stdexec::inplace_stop_source* stop_source, R receiver)
            : source(stop_source), inner(stdexec::connect(
                  stdexec::schedule(std::move(scheduler)), std::move(receiver)))
        {}
        void start() & noexcept
        {
            auto* stop_source = source;
            stdexec::start(inner);
            CHECK(!stop_source->request_stop());
        }
    };

    template<class R>
    auto connect(R receiver) const
    {
        return Operation<R>{scheduler, source, std::move(receiver)};
    }
};

stdexec::task<void> modern_explicit_schedule(Control& control)
{
    auto token = co_await stdexec::read_env(stdexec::get_stop_token);
    CHECK(token == control.source.get_token());
    auto current = co_await stdexec::read_env(stdexec::get_start_scheduler);
    static_assert(std::is_same_v<decltype(current), stdexec::task_scheduler>);
    co_await StopAfterQueueSender<decltype(current)>{current, &control.source};
    control.continued = true;
}

void test_queued_comparison()
{
    Control control;
    auto operation = stdexec::connect(stdexec::starts_on(control.loop.get_scheduler(),
        enqueue_comparison(control)), RootReceiver{&control});
    stdexec::start(operation);
    CHECK(control.root_completion == Completion::None);
    control.loop.run();
    CHECK(control.root_completion == Completion::Value);
    CHECK(control.direct.completion == Completion::Stopped);
    CHECK(control.task_scheduler.completion == Completion::Stopped);
    CHECK(control.old_task_scheduler.completion == Completion::Value);
    CHECK(control.direct.requested_at_completion);
    CHECK(control.task_scheduler.requested_at_completion);
    CHECK(control.old_task_scheduler.requested_at_completion);
}

void test_modern_task()
{
    Control control;
    auto operation = stdexec::connect(stdexec::starts_on(control.loop.get_scheduler(),
        modern_explicit_schedule(control)), RootReceiver{&control});
    stdexec::start(operation);
    control.loop.run();
    CHECK(control.source.get_token().stop_requested());
    CHECK(control.root_completion == Completion::Stopped);
    CHECK(!control.continued);
}
} // namespace

int main()
{
    for (int iteration = 0; iteration < 100; ++iteration) {
        test_queued_comparison();
        test_modern_task();
    }
    std::puts("PASS: 100 deterministic repetitions");
    std::puts("same inplace token: run_loop=stopped, task_scheduler(run_loop)=stopped,");
    std::puts("old exec::task current scheduler=value, inline_scheduler=value");
    std::puts("stdexec::task current scheduler (queued before stop)=stopped; continuation skipped");
}
