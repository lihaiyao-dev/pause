#include <exec/completion_behavior.hpp>
#include <exec/task.hpp>
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

// The host wrapper from design section 9.3.1. Ending the co_await full
// expression destroys its awaiter before this wrapper delivers a value.
exec::task<std::optional<int>> as_optional_task(exec::task<int> inner)
{
    auto result = co_await stdexec::stopped_as_optional(std::move(inner));
    co_return result;
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

void test_optional_task_frame_retirement()
{
    {
        Ownership state;
        std::atomic<int> entered{0};
        auto observed = as_optional_task(stopped_task(ResponsibilityGuard{state}, entered))
                      | stdexec::then([](std::optional<int> result) {
                            CHECK(!result.has_value());
                        });
        inspect_before_join(std::move(observed), state, 0);
        CHECK(entered.load() == 1);
    }
    {
        Ownership state;
        auto observed = as_optional_task(value_task(ResponsibilityGuard{state}))
                      | stdexec::then([](std::optional<int> result) {
                            CHECK(result.has_value() && *result == 42);
                        });
        inspect_before_join(std::move(observed), state, 0);
    }
    {
        Ownership state;
        auto recovered = stdexec::let_error(
            as_optional_task(error_task(ResponsibilityGuard{state})),
            [](std::exception_ptr error) {
                CHECK(error != nullptr);
                return stdexec::just(std::optional<int>{});
            });
        inspect_before_join(std::move(recovered), state, 0);
    }
}

// Stable ownership nodes model only fork's direct-parent lifetime constraint.
// They have no Pause gate, waiter, callback, or responsibility accounting.
struct TreeNode {
    int parent = -1;
    bool spine = false;
    std::atomic<bool> live{false};
    std::atomic<int> live_children{0};
    std::atomic<int> retired_order{0};
};

struct TreeOwnership {
    std::array<TreeNode, 64> nodes;
    int allocated = 0; // Only the workflow thread allocates nodes.
    std::atomic<int> live{0};
    std::atomic<int> retired{0};
    std::atomic<int> successful_joins{0};
};

struct TreeGuard {
    TreeOwnership* ownership;
    int index;

    TreeGuard(TreeOwnership& state, int parent = -1, bool spine = true) noexcept
        : ownership(&state), index(state.allocated++)
    {
        CHECK(index < static_cast<int>(state.nodes.size()));
        auto& node = state.nodes[index];
        node.parent = parent;
        node.spine = spine;
        node.live.store(true);
        ++state.live;
        if (parent != -1) {
            CHECK(state.nodes[parent].live.load());
            ++state.nodes[parent].live_children;
        }
    }
    TreeGuard(const TreeGuard&) = delete;
    TreeGuard& operator=(const TreeGuard&) = delete;
    TreeGuard(TreeGuard&& other) noexcept
        : ownership(std::exchange(other.ownership, nullptr)), index(other.index)
    {}
    TreeGuard& operator=(TreeGuard&&) = delete;
    ~TreeGuard()
    {
        if (ownership) {
            auto& node = ownership->nodes[index];
            CHECK(node.live_children.load() == 0);
            CHECK(node.live.exchange(false));
            node.retired_order.store(++ownership->retired);
            --ownership->live;
            if (node.parent != -1) {
                auto& parent = ownership->nodes[node.parent];
                CHECK(parent.live.load());
                CHECK(parent.live_children.fetch_sub(1) > 0);
            }
        }
    }
};

enum class TreeCompletion { Value, Error, Stopped };

exec::task<int> tree_sibling_task(TreeGuard guard)
{
    (void)guard;
    co_return 7;
}

exec::task<int> nested_fork_task(TreeGuard guard, int depth,
                                TreeCompletion completion, bool balanced)
{
    if (depth == 0) {
        if (completion == TreeCompletion::Stopped) {
            co_await stdexec::just_stopped();
            fail("a stopped tree leaf must not resume", __LINE__);
        }
        if (completion == TreeCompletion::Error)
            throw std::runtime_error("expected nested task error");
        co_return 42;
    }

    auto& state = *guard.ownership;
    TreeGuard left{state, guard.index, state.nodes[guard.index].spine};
    TreeGuard right{state, guard.index, false};
    // Each non-leaf task owns its parent guard until both direct child guards
    // retire. The recursive call creates another independent when_all scope.
    if (balanced) {
        co_await stdexec::when_all(
            nested_fork_task(std::move(left), depth - 1, completion, true),
            nested_fork_task(std::move(right), depth - 1, TreeCompletion::Value, true));
    } else {
        co_await stdexec::when_all(
            nested_fork_task(std::move(left), depth - 1, completion, false),
            tree_sibling_task(std::move(right)));
    }
    CHECK(state.nodes[guard.index].live_children.load() == 0);
    ++state.successful_joins;
    co_return 42;
}

void check_tree_retired(const TreeOwnership& state)
{
    CHECK(state.live.load() == 0);
    CHECK(state.retired.load() == state.allocated);
    for (int index = 0; index < state.allocated; ++index) {
        const auto& node = state.nodes[index];
        CHECK(!node.live.load());
        CHECK(node.live_children.load() == 0);
        CHECK(node.retired_order.load() > 0);
        if (node.parent != -1)
            CHECK(node.retired_order.load()
                  < state.nodes[node.parent].retired_order.load());
    }
}

void check_tree_shape(const TreeOwnership& state, int depth)
{
    int live = 0;
    int spine = 0;
    for (int index = 0; index < state.allocated; ++index) {
        const auto& node = state.nodes[index];
        CHECK(index == 0 ? node.parent == -1
                         : node.parent >= 0 && node.parent < index);
        int level = 0;
        for (int parent = node.parent; parent != -1; parent = state.nodes[parent].parent)
            ++level;
        CHECK(level <= depth);
        int children = 0;
        int live_children = 0;
        for (int child = index + 1; child < state.allocated; ++child) {
            if (state.nodes[child].parent == index) {
                ++children;
                live_children += state.nodes[child].live.load() ? 1 : 0;
            }
        }
        CHECK(children == 0 || children == 2);
        CHECK(node.live_children.load() == live_children);
        live += node.live.load() ? 1 : 0;
        spine += node.spine ? 1 : 0;
    }
    CHECK(spine == depth + 1);
    CHECK(state.live.load() == live);
    CHECK(state.retired.load() == state.allocated - live);
}

template<class Sender>
void inspect_tree_before_join(Sender sender, TreeOwnership& state,
                              int depth, bool retained, bool balanced = false)
{
    ManualCompletion sibling;
    std::latch first_completed{1};
    std::jthread runner([&] {
        auto first = std::move(sender) | stdexec::then([&](auto&&...) {
            first_completed.count_down();
        });
        CHECK(stdexec::sync_wait(stdexec::when_all(
            std::move(first), ManualSender{&sibling})).has_value());
    });
    first_completed.wait();
    sibling.published.wait();
    if (balanced) {
        // Error/stopped can prevent other subtrees from starting. Validate the
        // actual published tree instead of assuming every task body entered.
        CHECK(state.allocated >= 2 * depth + 1);
        CHECK(state.allocated <= (1 << (depth + 1)) - 1);
    } else {
        CHECK(state.allocated == 2 * depth + 1);
    }
    check_tree_shape(state, depth);
    if (retained) {
        CHECK(state.live.load() >= depth + 1);
        if (!balanced)
            CHECK(state.successful_joins.load() == 0);
        for (int index = 0; index < state.allocated; ++index) {
            if (state.nodes[index].spine)
                CHECK(state.nodes[index].live.load());
        }
    } else {
        check_tree_retired(state);
    }
    // Completion of this outside sibling is always under test-host control;
    // the stopped-frame counterexample cannot strand the test in a join.
    sibling.finish();
    runner.join();
    check_tree_retired(state);
}

void test_nested_fork_frame_retirement(int depth)
{
    {
        TreeOwnership state;
        inspect_tree_before_join(nested_fork_task(TreeGuard{state}, depth,
            TreeCompletion::Value, false), state, depth, false);
        CHECK(state.successful_joins.load() == depth);
    }
    {
        TreeOwnership state;
        auto recovered = stdexec::let_error(nested_fork_task(TreeGuard{state}, depth,
            TreeCompletion::Error, false), [](std::exception_ptr error) {
                CHECK(error != nullptr);
                return stdexec::just(0);
            });
        inspect_tree_before_join(std::move(recovered), state, depth, false);
        CHECK(state.successful_joins.load() == 0);
    }
    {
        TreeOwnership state;
        auto converted = stdexec::stopped_as_optional(nested_fork_task(TreeGuard{state},
            depth, TreeCompletion::Stopped, false))
                       | stdexec::then([](std::optional<int> result) {
                             CHECK(!result.has_value());
                         });
        inspect_tree_before_join(std::move(converted), state, depth, true);
    }
    {
        TreeOwnership state;
        auto converted = as_optional_task(nested_fork_task(TreeGuard{state}, depth,
            TreeCompletion::Stopped, false))
                       | stdexec::then([](std::optional<int> result) {
                             CHECK(!result.has_value());
                         });
        inspect_tree_before_join(std::move(converted), state, depth, false);
        CHECK(state.successful_joins.load() == 0);
    }
}

void test_balanced_fork_frame_retirement()
{
    constexpr int depth = 3;
    {
        TreeOwnership state;
        inspect_tree_before_join(nested_fork_task(TreeGuard{state}, depth,
            TreeCompletion::Value, true), state, depth, false, true);
        CHECK(state.allocated == (1 << (depth + 1)) - 1);
        CHECK(state.successful_joins.load() == (1 << depth) - 1);
    }
    {
        TreeOwnership state;
        auto recovered = stdexec::let_error(nested_fork_task(TreeGuard{state}, depth,
            TreeCompletion::Error, true), [](std::exception_ptr error) {
                CHECK(error != nullptr);
                return stdexec::just(0);
            });
        inspect_tree_before_join(std::move(recovered), state, depth, false, true);
    }
    {
        TreeOwnership state;
        auto converted = stdexec::stopped_as_optional(nested_fork_task(TreeGuard{state},
            depth, TreeCompletion::Stopped, true))
                       | stdexec::then([](std::optional<int> result) {
                             CHECK(!result.has_value());
                         });
        inspect_tree_before_join(std::move(converted), state, depth, true, true);
    }
    {
        TreeOwnership state;
        auto converted = as_optional_task(nested_fork_task(TreeGuard{state}, depth,
            TreeCompletion::Stopped, true))
                       | stdexec::then([](std::optional<int> result) {
                             CHECK(!result.has_value());
                         });
        inspect_tree_before_join(std::move(converted), state, depth, false, true);
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

exec::task<int> held_task(ResponsibilityGuard guard, ManualCompletion& control,
                         std::atomic<int>& entered)
{
    (void)guard;
    auto token = co_await stdexec::read_env(stdexec::get_stop_token);
    CHECK(token.stop_possible());
    ++entered;
    co_await ManualSender{&control};
    CHECK(token.stop_requested());
    auto scheduler = co_await stdexec::read_env(stdexec::get_start_scheduler);
    // Pinned task's erased scheduler omits the get_stop_token receiver query.
    // Even an explicit schedule on its advertised current scheduler succeeds
    // here. This characterizes the pinned task scheduler; the unconditional
    // stopped expectation in design section 14.3 needs qualification.
    co_await stdexec::schedule(scheduler);
    CHECK(token.stop_requested());
    co_return 42;
}

void test_optional_task_stop_visibility(bool pre_cancelled)
{
    Ownership state;
    std::atomic<int> entered{0};
    stdexec::inplace_stop_source source;
    if (pre_cancelled)
        CHECK(!source.request_stop());
    stdexec::run_loop scheduler;
    std::jthread scheduler_thread([&] { scheduler.run(); });
    ManualCompletion inner;
    StopObserverSender::Control sibling;
    std::latch wrapper_completed{1};
    bool wrapper_has_value = false;
    bool result_has_value = true;
    std::jthread runner([&] {
        auto scheduled = stdexec::starts_on(scheduler.get_scheduler(),
            as_optional_task(held_task(ResponsibilityGuard{state}, inner, entered)))
                       | stdexec::then([&](std::optional<int> result) {
                             CHECK(result.has_value() && *result == 42);
                             wrapper_has_value = true;
                             wrapper_completed.count_down();
                         });
        auto wrapper = stdexec::let_stopped(std::move(scheduled), [&] {
                wrapper_completed.count_down();
                return stdexec::just_stopped();
            });
        auto joined = stdexec::when_all(
            std::move(wrapper), StopObserverSender{&sibling});
        result_has_value = stdexec::sync_wait(stdexec::write_env(std::move(joined),
            stdexec::prop{stdexec::get_stop_token, source.get_token()})).has_value();
    });
    sibling.published.wait();
    if (!pre_cancelled) {
        inner.published.wait();
        CHECK(entered.load() == 1);
        CHECK(!source.request_stop());
        inner.finish();
    }
    sibling.stop_seen.wait();
    wrapper_completed.wait();
    // The concrete starts_on scheduler accepts pre-cancellation. Once started,
    // task's erased current scheduler does not forward the requested token.
    CHECK(entered.load() == (pre_cancelled ? 0 : 1));
    CHECK(wrapper_has_value == !pre_cancelled);
    if (!pre_cancelled)
        CHECK(state.live.load() == 0);
    sibling.complete(sibling.operation);
    runner.join();
    scheduler.finish();
    scheduler_thread.join();
    CHECK(!result_has_value);
    CHECK(state.live.load() == 0);
    CHECK(state.retired.load() == 1);
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
        test_task_frame_retirement();
        test_optional_task_frame_retirement();
        test_nested_fork_frame_retirement(3);
        test_nested_fork_frame_retirement(5);
        test_balanced_fork_frame_retirement();
        test_no_guard_means_no_owned_responsibility();
        test_unswallowed_completion_cancels_sibling();
        test_optional_task_stop_visibility(true);
        test_optional_task_stop_visibility(false);
        test_task_start_completion_handshake(false);
        test_task_start_completion_handshake(true);
    }
    std::printf("stdexec task lifetime integration: %d cases passed (%d repetitions)\n",
                iterations * 25, iterations);
}
