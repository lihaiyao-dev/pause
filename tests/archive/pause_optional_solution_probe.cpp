/* 测试项目：pause_optional_solution_probe.cpp
 * 历史 exec::task 的 optional_task 清理实验, 不调用真实 Pause.
 * main 循环 1000 次, 每轮 stopped 转空 optional/value 42/error 被 let_error 接收, 共 3000 项.
 * 预期: 兄弟 sender 未完成时被包裹 task 的责任 guard 已退役, 最终仅退役一次.
 * 入口: 手动提供 exec/task.hpp 和 exec/completion_behavior.hpp 的历史 stdexec 编译运行, 无 CLI 参数.
 * 边界: 保留的其他 test_* 函数未被当前 main 调用, 不计覆盖; 不代表当前 stdexec::task/Pause 保证.
 * 归档文件, 未注册 CTest; 保留历史依赖与 API 前提.
 */
#include <exec/completion_behavior.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <latch>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

// These are integration experiments against the pinned stdexec version.
// ResponsibilityGuard models ownership only. It is not a Pause implementation.
namespace {

[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::exit(EXIT_FAILURE);
}

#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

struct Ownership {
    std::atomic<int> live{0};
    std::atomic<int> retired{0};
};

struct ResponsibilityGuard {
    Ownership* ownership;

    explicit ResponsibilityGuard(Ownership& state) noexcept : ownership(&state)
    {
        ++ownership->live;
    }
    ResponsibilityGuard(const ResponsibilityGuard&) = delete;
    ResponsibilityGuard& operator=(const ResponsibilityGuard&) = delete;
    ResponsibilityGuard(ResponsibilityGuard&& other) noexcept
        : ownership(std::exchange(other.ownership, nullptr))
    {}
    ResponsibilityGuard& operator=(ResponsibilityGuard&&) = delete;
    ~ResponsibilityGuard()
    {
        if (ownership) {
            --ownership->live;
            ++ownership->retired;
        }
    }
};

struct ManualCompletion {
    std::latch published{1};
    void* operation = nullptr;
    void (*complete)(void*) noexcept = nullptr;

    void finish() noexcept
    {
        // The published latch makes operation/complete visible. The caller
        // performs exactly one completion and keeps this control block alive.
        published.wait();
        complete(operation);
    }
};

struct ManualSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t()>;
    ManualCompletion* control;

    template<class Receiver>
    struct Operation {
        ManualCompletion* control;
        Receiver receiver;

        void start() & noexcept
        {
            auto* local = control;
            local->operation = this;
            local->complete = [](void* pointer) noexcept {
                auto* self = static_cast<Operation*>(pointer);
                stdexec::set_value(std::move(self->receiver));
            };
            local->published.count_down();
        }
    };

    template<class Receiver>
    auto connect(Receiver receiver) const
    {
        return Operation<Receiver>{control, std::move(receiver)};
    }
};

exec::task<int> stopped_task(ResponsibilityGuard guard, std::atomic<int>& entered)
{
    (void)guard;
    ++entered; // Represents reaching an entry point, not a real Pause operation.
    co_await stdexec::just_stopped();
    fail("a stopped task must not resume", __LINE__);
    co_return 0;
}

exec::task<int> untracked_stopped_task(std::atomic<int>& entered)
{
    ++entered;
    co_await stdexec::just_stopped();
    fail("a stopped task must not resume", __LINE__);
    co_return 0;
}

exec::task<int> value_task(ResponsibilityGuard guard)
{
    (void)guard;
    co_return 42;
}

exec::task<int> error_task(ResponsibilityGuard guard)
{
    (void)guard;
    throw std::runtime_error("expected task error");
    co_return 0;
}

template<class Sender>
void inspect_before_join(Sender sender, Ownership& ownership, int expected_live)
{
    ManualCompletion sibling;
    std::latch first_completed{1};
    std::exception_ptr thread_error;
    bool join_has_value = false;
    std::jthread runner([&] {
        try {
            auto first = std::move(sender) | stdexec::then([&](auto&&...) {
                first_completed.count_down();
            });
            join_has_value = stdexec::sync_wait(stdexec::when_all(
                std::move(first), ManualSender{&sibling})).has_value();
        } catch (...) {
            thread_error = std::current_exception();
            // Do not hang the coordinator if setup fails before publication.
            std::fprintf(stderr, "unexpected exception in lifetime experiment\n");
            std::exit(EXIT_FAILURE);
        }
    });

    first_completed.wait();
    sibling.published.wait();
    CHECK(ownership.live.load() == expected_live);
    CHECK(ownership.retired.load() == 1 - expected_live);
    sibling.finish(); // Always release the join; never reproduce a permanent wait.
    runner.join();
    CHECK(!thread_error);
    CHECK(join_has_value);
    CHECK(ownership.live.load() == 0);
    CHECK(ownership.retired.load() == 1);
}

void test_task_frame_retirement()
{
    {
        Ownership state;
        std::atomic<int> entered{0};
        inspect_before_join(stdexec::stopped_as_optional(
            stopped_task(ResponsibilityGuard{state}, entered)), state, 1);
        CHECK(entered.load() == 1);
    }
    {
        Ownership state;
        inspect_before_join(value_task(ResponsibilityGuard{state}), state, 0);
    }
    {
        Ownership state;
        auto recovered = stdexec::let_error(
            error_task(ResponsibilityGuard{state}), [](std::exception_ptr error) {
                CHECK(error != nullptr);
                return stdexec::just(0);
            });
        inspect_before_join(std::move(recovered), state, 0);
    }
}

void test_no_guard_means_no_owned_responsibility()
{
    Ownership state;
    std::atomic<int> entered{0};
    ManualCompletion sibling;
    std::latch first_completed{1};
    std::jthread runner([&] {
        auto first = stdexec::stopped_as_optional(untracked_stopped_task(entered))
                   | stdexec::then([&](std::optional<int> value) {
                         CHECK(!value.has_value());
                         first_completed.count_down();
                     });
        CHECK(stdexec::sync_wait(stdexec::when_all(
            std::move(first), ManualSender{&sibling})).has_value());
    });
    first_completed.wait();
    sibling.published.wait();
    CHECK(entered.load() == 1);
    CHECK(state.live.load() == 0);
    CHECK(state.retired.load() == 0);
    sibling.finish();
    runner.join();
}

struct StopObserverSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(), stdexec::set_stopped_t()>;
    struct Control {
        std::latch published{1};
        std::latch stop_seen{1};
        void* operation = nullptr;
        void (*complete)(void*) noexcept = nullptr;
    };

    struct OnStop {
        Control* control;
        void operator()() const noexcept { control->stop_seen.count_down(); }
    };

    Control* control;

    template<class Receiver>
    struct Operation {
        Control* control;
        Receiver receiver;
        std::optional<stdexec::inplace_stop_callback<OnStop>> callback;

        void start() & noexcept
        {
            auto* local = control;
            auto token = stdexec::get_stop_token(stdexec::get_env(receiver));
            // Pre-stopped tokens can invoke OnStop inside emplace. It only
            // signals the coordinator; completion waits for publication below.
            callback.emplace(token, OnStop{local});
            local->operation = this;
            local->complete = [](void* pointer) noexcept {
                auto* self = static_cast<Operation*>(pointer);
                self->callback.reset();
                stdexec::set_stopped(std::move(self->receiver));
            };
            local->published.count_down();
        }
    };

    template<class Receiver>
    auto connect(Receiver receiver) const
    {
        return Operation<Receiver>{control, std::move(receiver), {}};
    }
};

void test_unswallowed_completion_cancels_sibling()
{
    {
        Ownership state;
        std::atomic<int> entered{0};
        StopObserverSender::Control sibling;
        bool result_has_value = true;
        std::jthread runner([&] {
            result_has_value = stdexec::sync_wait(stdexec::when_all(
                stopped_task(ResponsibilityGuard{state}, entered),
                StopObserverSender{&sibling})).has_value();
        });
        sibling.published.wait();
        sibling.stop_seen.wait();
        CHECK(state.live.load() == 1);
        sibling.complete(sibling.operation);
        runner.join();
        CHECK(!result_has_value);
        CHECK(state.live.load() == 0);
        CHECK(state.retired.load() == 1);
    }
    {
        Ownership state;
        StopObserverSender::Control sibling;
        bool received_error = false;
        std::jthread runner([&] {
            try {
                (void)stdexec::sync_wait(stdexec::when_all(
                    error_task(ResponsibilityGuard{state}),
                    StopObserverSender{&sibling}));
            } catch (const std::runtime_error&) {
                received_error = true;
            }
        });
        sibling.published.wait();
        sibling.stop_seen.wait();
        CHECK(state.live.load() == 0);
        sibling.complete(sibling.operation);
        runner.join();
        CHECK(received_error);
        CHECK(state.live.load() == 0);
        CHECK(state.retired.load() == 1);
    }
}

struct Handshake {
    std::latch published{1};
    std::latch receiver_entered{1};
    std::latch allow_start_return{1};
    std::atomic<bool> start_tail_finished{false};
    std::atomic<bool> completion_returned{false};
    std::atomic<bool> continued{false};
    std::atomic<bool> observed_stopped{false};
    std::atomic<bool> probe_reported{false};
    void* operation = nullptr;
    void (*complete)(void*, bool) noexcept = nullptr;
};

struct CompletionProbe {
    Handshake* state;
    explicit CompletionProbe(Handshake* value) noexcept : state(value) {}
    CompletionProbe(const CompletionProbe&) = delete;
    CompletionProbe(CompletionProbe&& other) noexcept : state(other.state)
    {
        // The first move takes place in stdexec's real await receiver while
        // storing its value. This is stronger than signalling before set_value.
        if (!state->probe_reported.exchange(true)) {
            state->receiver_entered.count_down();
        }
    }
};

struct AffineAttrs {
    template<class Env>
    constexpr auto query(exec::get_completion_behavior_t<stdexec::set_value_t>,
                         const Env&) const noexcept
    {
        return exec::completion_behavior::inline_completion
             | exec::completion_behavior::asynchronous_affine;
    }
};

struct HeldStartSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(CompletionProbe), stdexec::set_stopped_t()>;
    Handshake* state;

    AffineAttrs get_env() const noexcept { return {}; }

    template<class Receiver>
    struct Operation {
        Handshake* state;
        Receiver receiver;

        void start() & noexcept
        {
            auto* local = state;
            local->operation = this;
            local->complete = [](void* pointer, bool stop) noexcept {
                auto* self = static_cast<Operation*>(pointer);
                auto* local_state = self->state;
                if (stop) {
                    // stopped has no value-construction hook. This latch marks
                    // an attempted delivery, not entry into stdexec's spin loop.
                    local_state->receiver_entered.count_down();
                    stdexec::set_stopped(std::move(self->receiver));
                } else {
                    stdexec::set_value(std::move(self->receiver),
                                       CompletionProbe{local_state});
                }
                // Only the independently owned control block is accessed here.
                local_state->completion_returned.store(true);
            };
            local->published.count_down();
            local->allow_start_return.wait();
            local->start_tail_finished.store(true);
        }
    };

    template<class Receiver>
    auto connect(Receiver receiver) const
    {
        return Operation<Receiver>{state, std::move(receiver)};
    }
};

exec::task<void> handshake_task(Handshake& state)
{
    auto value = co_await HeldStartSender{&state};
    CHECK(value.state == &state);
    CHECK(state.start_tail_finished.load());
    state.continued.store(true);
}

void test_task_start_completion_handshake(bool stop)
{
    Handshake state;
    std::jthread runner([&] {
        // inline_scheduler has no OS-thread identity requirement. Its execution
        // resource permits the external completion thread used in this test.
        auto sender = stdexec::starts_on(stdexec::inline_scheduler{},
                                        handshake_task(state))
                    | stdexec::upon_stopped([&] {
                          CHECK(state.start_tail_finished.load());
                          state.observed_stopped.store(true);
                      });
        CHECK(stdexec::sync_wait(std::move(sender)).has_value());
    });
    state.published.wait();
    std::jthread completer([&] { state.complete(state.operation, stop); });
    state.receiver_entered.wait();
    CHECK(!state.start_tail_finished.load());
    CHECK(!state.continued.load());
    CHECK(!state.observed_stopped.load());
    state.allow_start_return.count_down();
    completer.join();
    runner.join();
    CHECK(state.completion_returned.load());
    CHECK(state.continued.load() == !stop);
    CHECK(state.observed_stopped.load() == stop);
}

} // namespace

template<class T>
exec::task<std::optional<T>> optional_task(exec::task<T> inner)
{
    auto result = co_await stdexec::stopped_as_optional(std::move(inner));
    co_return result;
}

int main()
{
    for (int iteration = 0; iteration < 1000; ++iteration) {
        {
            Ownership state;
            std::atomic<int> entered{0};
            auto observed = optional_task(stopped_task(ResponsibilityGuard{state}, entered))
                | stdexec::then([](std::optional<int> result) {
                    CHECK(!result.has_value());
                });
            inspect_before_join(std::move(observed), state, 0);
            CHECK(entered.load() == 1);
        }
        {
            Ownership state;
            auto observed = optional_task(value_task(ResponsibilityGuard{state}))
                | stdexec::then([](std::optional<int> result) {
                    CHECK(result.has_value() && *result == 42);
                });
            inspect_before_join(std::move(observed), state, 0);
        }
        {
            Ownership state;
            auto recovered = stdexec::let_error(
                optional_task(error_task(ResponsibilityGuard{state})),
                [](std::exception_ptr error) {
                    CHECK(error != nullptr);
                    return stdexec::just(std::optional<int>{});
                });
            inspect_before_join(std::move(recovered), state, 0);
        }
    }
    std::puts("optional_task cleanup boundary: 3000 cases passed; A guard retired before B completes");
}