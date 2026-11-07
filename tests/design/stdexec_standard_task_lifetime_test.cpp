/* 测试项目：stdexec_standard_task_lifetime_test
 * 目的: 用所有权哨兵检验固定版本新 stdexec::task 的 frame、子任务与操作生命周期。
 * 测试项:
 * 1. 深度 0/3/5 的侧支链及深度 3 的二叉分支, 各核对 value、error、
 *    外包 stopped_as_optional 和 task wrapper 内部 stopped_as_optional 四条路径。
 * 2. wrapper 的 value/error 传播; 原始 stopped/error 对未完成兄弟的取消。
 * 3. wrapper 在宿主 starts_on 前预取消时不进入业务协程, 最终释放所有持有责任。
 * 4. 异线程 completion 先进入而 start 尚未返回的 value/stopped 握手。
 * 预期: value/error 与 wrapper 转换停止后, 责任在无关兄弟完成前释放;
 *       直接在 task 外转换 stopped 及协程包装路径都应在无关兄弟完成前释放 frame;
 *       各节点只退役一次, 父子计数与次序正确, start 尾部结束前不继续业务或发布 stopped。
 * 入口: 配置 PAUSE_BUILD_DESIGN_TESTS=ON, CTest 名称 stdexec_standard_task_lifetime_test。
 *       CLI --iterations N, 默认 100, 范围 1..1000000, 每轮 23 个场景。
 * 边界: 使用真实新 task 和测试用 Guard, 不含 pause.hpp, 不证明 Pause 的真实分支计数。
 *       二叉分支仅正常路径保证满展开; 测试固定浅层拓扑, 不作为任意深度栈安全证明。
 */
#include <exec/completion_behavior.hpp>
#include <stdexec/execution.hpp>

#include <array>
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

// Dependency experiments for the pinned stdexec::task. The stable ownership
// tree is a lifetime sentinel, not a Pause implementation or a fork API.
namespace {
[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

struct Node {
    int parent = -1;
    bool spine = false;
    std::atomic<bool> live{false};
    std::atomic<int> children{0};
    std::atomic<int> retired_order{0};
};

struct Ownership {
    std::array<Node, 64> nodes;
    int allocated = 0; // Allocation happens on one workflow thread.
    std::atomic<int> live{0};
    std::atomic<int> retired{0};
    std::atomic<int> entered{0};
    std::atomic<int> successful_joins{0};

    void inspect(int depth, bool balanced) const
    {
        CHECK(allocated >= 2 * depth + 1);
        CHECK(allocated <= (balanced ? (1 << (depth + 1)) - 1 : 2 * depth + 1));
        int observed_live = 0;
        int observed_spine = 0;
        for (int index = 0; index < allocated; ++index) {
            const auto& node = nodes[index];
            CHECK(index == 0 ? node.parent == -1
                             : node.parent >= 0 && node.parent < index);
            int level = 0;
            for (int parent = node.parent; parent != -1; parent = nodes[parent].parent)
                ++level;
            CHECK(level <= depth);
            int all_children = 0;
            int live_children = 0;
            for (int child = index + 1; child < allocated; ++child) {
                if (nodes[child].parent == index) {
                    ++all_children;
                    live_children += nodes[child].live.load() ? 1 : 0;
                }
            }
            CHECK(all_children == 0 || all_children == 2);
            CHECK(node.children.load() == live_children);
            observed_live += node.live.load() ? 1 : 0;
            observed_spine += node.spine ? 1 : 0;
        }
        CHECK(observed_spine == depth + 1);
        CHECK(live.load() == observed_live);
        CHECK(retired.load() == allocated - observed_live);
        check_retired();
    }

    void check_retired() const
    {
        CHECK(live.load() == 0);
        CHECK(retired.load() == allocated);
        for (int index = 0; index < allocated; ++index) {
            const auto& node = nodes[index];
            CHECK(!node.live.load());
            CHECK(node.children.load() == 0);
            CHECK(node.retired_order.load() > 0);
            if (node.parent != -1)
                CHECK(node.retired_order.load() < nodes[node.parent].retired_order.load());
        }
    }
};

struct Guard {
    Ownership* state;
    int index;

    explicit Guard(Ownership& ownership, int parent = -1, bool spine = true) noexcept
        : state(&ownership), index(ownership.allocated++)
    {
        CHECK(index < static_cast<int>(state->nodes.size()));
        auto& node = state->nodes[index];
        node.parent = parent;
        node.spine = spine;
        node.live.store(true);
        ++state->live;
        if (parent != -1) {
            CHECK(state->nodes[parent].live.load());
            ++state->nodes[parent].children;
        }
    }
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;
    Guard(Guard&& other) noexcept
        : state(std::exchange(other.state, nullptr)), index(other.index)
    {}
    Guard& operator=(Guard&&) = delete;
    ~Guard()
    {
        if (state) {
            auto& node = state->nodes[index];
            CHECK(node.children.load() == 0);
            CHECK(node.live.exchange(false));
            node.retired_order.store(++state->retired);
            --state->live;
            if (node.parent != -1) {
                CHECK(state->nodes[node.parent].live.load());
                CHECK(state->nodes[node.parent].children.fetch_sub(1) > 0);
            }
        }
    }
};

struct Control {
    std::latch published{1};
    std::latch stop_seen{1};
    void* operation = nullptr;
    void (*complete)(void*, bool) noexcept = nullptr;

    void finish(bool stopped = false) noexcept
    {
        published.wait();
        complete(operation, stopped);
    }
};

template<bool ObserveStop = false>
struct ControlledSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(), stdexec::set_stopped_t()>;
    Control* control;

    struct OnStop {
        Control* control;
        void operator()() const noexcept { control->stop_seen.count_down(); }
    };

    template<class Receiver>
    struct Operation {
        Control* control;
        Receiver receiver;
        std::optional<stdexec::inplace_stop_callback<OnStop>> callback;

        void start() & noexcept
        {
            auto* local = control;
            if constexpr (ObserveStop)
                callback.emplace(stdexec::get_stop_token(stdexec::get_env(receiver)),
                                 OnStop{local});
            local->operation = this;
            local->complete = [](void* pointer, bool stopped) noexcept {
                auto* self = static_cast<Operation*>(pointer);
                self->callback.reset();
                if (stopped)
                    stdexec::set_stopped(std::move(self->receiver));
                else
                    stdexec::set_value(std::move(self->receiver));
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

enum class Outcome { Value, Error, Stopped };

stdexec::task<int> branch_task(Guard guard, int depth, bool balanced, Outcome outcome)
{
    auto& state = *guard.state;
    ++state.entered;
    if (depth == 0) {
        if (outcome == Outcome::Error)
            throw std::runtime_error("expected standard task error");
        if (outcome == Outcome::Stopped) {
            co_await stdexec::just_stopped();
            fail("a stopped leaf must not resume", __LINE__);
        }
        co_return 42;
    }
    Guard left{state, guard.index, state.nodes[guard.index].spine};
    Guard right{state, guard.index, false};
    co_await stdexec::when_all(
        branch_task(std::move(left), depth - 1, balanced, outcome),
        branch_task(std::move(right), balanced ? depth - 1 : 0, balanced, Outcome::Value));
    CHECK(state.nodes[guard.index].children.load() == 0);
    ++state.successful_joins;
    co_return 42;
}

stdexec::task<std::optional<int>> optional_task(stdexec::task<int> inner)
{
    auto result = co_await stdexec::stopped_as_optional(std::move(inner));
    co_return result;
}

void check_expected_error(std::exception_ptr error)
{
    CHECK(error != nullptr);
    try {
        std::rethrow_exception(error);
    } catch (const std::runtime_error& caught) {
        CHECK(std::strcmp(caught.what(), "expected standard task error") == 0);
        return;
    }
    fail("expected runtime_error", __LINE__);
}

template<class Sender>
void inspect_before_sibling(Sender sender, Ownership& state, int depth,
                            bool balanced)
{
    Control sibling;
    std::latch first_completed{1};
    std::jthread runner([&] {
        auto first = std::move(sender) | stdexec::then([&](auto&&...) {
            first_completed.count_down();
        });
        CHECK(stdexec::sync_wait(stdexec::when_all(
            std::move(first), ControlledSender<>{&sibling})).has_value());
    });
    first_completed.wait();
    sibling.published.wait();
    state.inspect(depth, balanced);
    sibling.finish();
    runner.join();
    state.check_retired();
}

void test_frame_paths(int depth, bool balanced)
{
    {
        Ownership state;
        inspect_before_sibling(branch_task(Guard{state}, depth, balanced, Outcome::Value),
                               state, depth, balanced);
        CHECK(state.allocated == (balanced ? (1 << (depth + 1)) - 1 : 2 * depth + 1));
        CHECK(state.successful_joins.load() == (balanced ? (1 << depth) - 1 : depth));
    }
    {
        Ownership state;
        auto recovered = stdexec::let_error(
            branch_task(Guard{state}, depth, balanced, Outcome::Error),
            [](std::exception_ptr error) {
                check_expected_error(error);
                return stdexec::just(0);
            });
        inspect_before_sibling(std::move(recovered), state, depth, balanced);
    }
    {
        Ownership state;
        auto converted = stdexec::stopped_as_optional(
            branch_task(Guard{state}, depth, balanced, Outcome::Stopped))
                       | stdexec::then([](std::optional<int> result) { CHECK(!result); });
        inspect_before_sibling(std::move(converted), state, depth, balanced);
    }
    {
        Ownership state;
        auto converted = optional_task(branch_task(Guard{state}, depth, balanced, Outcome::Stopped))
                       | stdexec::then([](std::optional<int> result) { CHECK(!result); });
        inspect_before_sibling(std::move(converted), state, depth, balanced);
    }
}

void test_wrapper_value_and_error()
{
    {
        Ownership state;
        auto wrapped = optional_task(branch_task(Guard{state}, 0, false, Outcome::Value))
                     | stdexec::then([](std::optional<int> result) {
                           CHECK(result && *result == 42);
                       });
        inspect_before_sibling(std::move(wrapped), state, 0, false);
    }
    {
        Ownership state;
        auto recovered = stdexec::let_error(
            optional_task(branch_task(Guard{state}, 0, false, Outcome::Error)),
            [](std::exception_ptr error) {
                check_expected_error(error);
                return stdexec::just(std::optional<int>{});
            });
        inspect_before_sibling(std::move(recovered), state, 0, false);
    }
}

void test_sibling_cancellation(Outcome outcome)
{
    Ownership state;
    Control sibling;
    bool received_error = false;
    bool has_value = true;
    std::jthread runner([&] {
        try {
            has_value = stdexec::sync_wait(stdexec::when_all(
                branch_task(Guard{state}, 0, false, outcome),
                ControlledSender<true>{&sibling})).has_value();
        } catch (const std::runtime_error& error) {
            CHECK(std::strcmp(error.what(), "expected standard task error") == 0);
            received_error = true;
        }
    });
    sibling.published.wait();
    sibling.stop_seen.wait();
    CHECK(state.live.load() == 0);
    sibling.finish(true);
    runner.join();
    CHECK(received_error == (outcome == Outcome::Error));
    if (outcome == Outcome::Stopped)
        CHECK(!has_value);
    state.check_retired();
}

void test_pre_cancelled_wrapper()
{
    Ownership state;
    stdexec::inplace_stop_source source;
    CHECK(source.request_stop());
    stdexec::run_loop scheduler;
    std::jthread scheduler_thread([&] { scheduler.run(); });
    Control sibling;
    std::latch wrapper_stopped{1};
    bool has_value = true;
    std::jthread runner([&] {
        auto first = stdexec::let_stopped(stdexec::starts_on(scheduler.get_scheduler(),
            optional_task(branch_task(Guard{state}, 0, false, Outcome::Value))), [&] {
                wrapper_stopped.count_down();
                return stdexec::just_stopped();
            });
        has_value = stdexec::sync_wait(stdexec::write_env(stdexec::when_all(
            std::move(first), ControlledSender<true>{&sibling}),
            stdexec::prop{stdexec::get_stop_token, source.get_token()})).has_value();
    });
    sibling.published.wait();
    sibling.stop_seen.wait();
    wrapper_stopped.wait();
    CHECK(state.entered.load() == 0);
    sibling.finish(true);
    runner.join();
    scheduler.finish();
    scheduler_thread.join();
    CHECK(!has_value);
    state.check_retired();
}

struct Handshake {
    std::latch published{1};
    std::latch receiver_entered{1};
    std::latch allow_start_return{1};
    std::atomic<bool> start_finished{false};
    std::atomic<bool> completion_returned{false};
    std::atomic<bool> continued{false};
    std::atomic<bool> stopped{false};
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
        if (!state->probe_reported.exchange(true))
            state->receiver_entered.count_down();
    }
};

struct AffineAttrs {
    // New task's affine adaptor checks all completion channels, unlike the
    // old task's value-only decision. Inline's resource allows either thread.
    template<class Tag, class Env>
    constexpr auto query(exec::get_completion_behavior_t<Tag>, const Env&) const noexcept
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
            local->complete = [](void* pointer, bool stopped) noexcept {
                auto* self = static_cast<Operation*>(pointer);
                auto* control = self->state;
                if (stopped) {
                    // No stopped value-construction hook exists: this marks
                    // attempted delivery, not arrival in the library spin loop.
                    control->receiver_entered.count_down();
                    stdexec::set_stopped(std::move(self->receiver));
                } else {
                    stdexec::set_value(std::move(self->receiver), CompletionProbe{control});
                }
                control->completion_returned.store(true); // Independent storage only.
            };
            local->published.count_down();
            local->allow_start_return.wait();
            local->start_finished.store(true);
        }
    };

    template<class Receiver>
    auto connect(Receiver receiver) const
    {
        return Operation<Receiver>{state, std::move(receiver)};
    }
};

stdexec::task<void> handshake_task(Guard guard, Handshake& state)
{
    (void)guard;
    auto value = co_await HeldStartSender{&state};
    CHECK(value.state == &state);
    CHECK(state.start_finished.load());
    state.continued.store(true);
}

void test_start_completion_handshake(bool stopped)
{
    Ownership ownership;
    Handshake state;
    std::jthread runner([&] {
        auto sender = stdexec::starts_on(stdexec::inline_scheduler{},
            handshake_task(Guard{ownership}, state)) | stdexec::upon_stopped([&] {
                CHECK(state.start_finished.load());
                state.stopped.store(true);
            });
        CHECK(stdexec::sync_wait(std::move(sender)).has_value());
    });
    state.published.wait();
    std::jthread completer([&] { state.complete(state.operation, stopped); });
    state.receiver_entered.wait();
    CHECK(!state.start_finished.load());
    CHECK(!state.continued.load());
    CHECK(!state.stopped.load());
    CHECK(ownership.live.load() == 1);
    state.allow_start_return.count_down();
    completer.join();
    runner.join();
    CHECK(state.completion_returned.load());
    CHECK(state.continued.load() == !stopped);
    CHECK(state.stopped.load() == stopped);
    ownership.check_retired();
}
} // namespace

int main(int argc, char** argv)
{
    int iterations = 100;
    if (argc != 1) {
        CHECK(argc == 3 && std::strcmp(argv[1], "--iterations") == 0);
        char* end = nullptr;
        const long parsed = std::strtol(argv[2], &end, 10);
        CHECK(end != argv[2] && *end == '\0' && parsed > 0 && parsed <= 1000000);
        iterations = static_cast<int>(parsed);
    }
    for (int iteration = 0; iteration < iterations; ++iteration) {
        test_frame_paths(0, false);
        test_wrapper_value_and_error();
        test_frame_paths(3, false);
        test_frame_paths(5, false);
        test_frame_paths(3, true);
        test_sibling_cancellation(Outcome::Stopped);
        test_sibling_cancellation(Outcome::Error);
        test_pre_cancelled_wrapper();
        test_start_completion_handshake(false);
        test_start_completion_handshake(true);
    }
    std::printf("stdexec::task lifetime dependency experiments: %d cases passed (%d repetitions)\n",
                iterations * 23, iterations);
}
