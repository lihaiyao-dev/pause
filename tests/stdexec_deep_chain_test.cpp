#include <stdexec/execution.hpp>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

// Pinned stdexec::task/run_loop dependency probe, not a Pause implementation.
// Storage and topology construction are O(depth), with no recursive test oracle.
// Do not increase the process/thread stack: recursive library limits are evidence.
namespace {
[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

enum class Topology { Chain, Fork };
enum class Outcome { Value, Error, Stopped, RootStop, ErrorFast, StoppedFast };

bool is_error(Outcome outcome) noexcept
{
    return outcome == Outcome::Error || outcome == Outcome::ErrorFast;
}

bool is_stopped(Outcome outcome) noexcept
{
    return outcome == Outcome::Stopped || outcome == Outcome::StoppedFast;
}

bool is_fast(Outcome outcome) noexcept
{
    return outcome == Outcome::ErrorFast || outcome == Outcome::StoppedFast;
}

const char* name(Topology topology) noexcept
{
    return topology == Topology::Chain ? "chain" : "fork";
}

const char* name(Outcome outcome) noexcept
{
    switch (outcome) {
    case Outcome::Value: return "value";
    case Outcome::Error: return "error";
    case Outcome::Stopped: return "stopped";
    case Outcome::RootStop: return "root-stop";
    case Outcome::ErrorFast: return "error-fast";
    case Outcome::StoppedFast: return "stopped-fast";
    }
    std::terminate();
}

void phase(const char* value)
{
    std::printf("phase=%s\n", value);
    std::fflush(stdout);
}

struct Node {
    int parent = -1;
    std::atomic<int> constructed{0};
    std::atomic<int> entered{0};
    std::atomic<int> retired{0};
    std::atomic<int> retired_order{0};
    std::atomic<int> children{0};
    std::atomic<bool> live{false};
};

struct Ownership {
    const int depth;
    const Topology topology;
    const int node_count;
    const int leaf_count;
    std::unique_ptr<Node[]> nodes;
    std::latch all_entered;
    std::latch all_published;
    std::latch all_stop_seen;
    std::atomic<int> live{0};
    std::atomic<int> retired{0};
    std::atomic<int> live_operations{0};
    std::atomic<int> operations_started{0};
    std::atomic<int> operations_retired{0};
    std::atomic<int> successful_joins{0};
    std::atomic<int> business{0};
    std::atomic<int> stop_callbacks{0};

    Ownership(int levels, Topology shape)
        : depth(levels), topology(shape),
          node_count(shape == Topology::Chain ? levels + 1 : 2 * levels + 1),
          leaf_count(shape == Topology::Chain ? 1 : levels + 1),
          nodes(std::make_unique<Node[]>(node_count)), all_entered(node_count),
          all_published(leaf_count), all_stop_seen(leaf_count)
    {
        for (int index = 1; index <= depth; ++index)
            nodes[index].parent = index - 1;
        if (topology == Topology::Fork) {
            for (int level = 0; level < depth; ++level)
                nodes[depth + 1 + level].parent = level;
        }
    }

    void check_all_started() const
    {
        CHECK(live.load() == node_count);
        CHECK(retired.load() == 0);
        CHECK(operations_started.load() == leaf_count);
        CHECK(live_operations.load() == leaf_count);
        for (int index = 0; index < node_count; ++index) {
            const auto& node = nodes[index];
            CHECK(node.constructed.load() == 1);
            CHECK(node.entered.load() == 1);
            CHECK(node.live.load());
            CHECK(node.retired.load() == 0);
            const int expected = index < depth ? (topology == Topology::Chain ? 1 : 2) : 0;
            CHECK(node.children.load() == expected);
        }
    }

    void check_retired() const
    {
        CHECK(live.load() == 0);
        CHECK(retired.load() == node_count);
        CHECK(live_operations.load() == 0);
        CHECK(operations_started.load() == leaf_count);
        CHECK(operations_retired.load() == leaf_count);
        for (int index = 0; index < node_count; ++index) {
            const auto& node = nodes[index];
            CHECK(node.constructed.load() == 1);
            CHECK(node.entered.load() == 1);
            CHECK(node.retired.load() == 1);
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

    Guard(Ownership& ownership, int node) noexcept : state(&ownership), index(node)
    {
        auto& record = state->nodes[index];
        CHECK(record.constructed.fetch_add(1) == 0);
        CHECK(!record.live.exchange(true));
        ++state->live;
        if (record.parent != -1) {
            CHECK(state->nodes[record.parent].live.load());
            ++state->nodes[record.parent].children;
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
            CHECK(node.retired.fetch_add(1) == 0);
            node.retired_order.store(state->retired.fetch_add(1) + 1);
            --state->live;
            if (node.parent != -1) {
                CHECK(state->nodes[node.parent].live.load());
                CHECK(state->nodes[node.parent].children.fetch_sub(1) > 0);
            }
        }
    }
};

struct Control {
    Ownership* state = nullptr;
    std::latch published{1};
    std::latch stop_seen{1};
    std::atomic<int> stop_calls{0};
    std::atomic<int> finish_calls{0};
    void* operation = nullptr;
    void (*complete)(void*, bool) noexcept = nullptr;

    void finish(bool stopped = false) noexcept
    {
        published.wait();
        CHECK(finish_calls.fetch_add(1) == 0);
        complete(operation, stopped);
    }
};

struct ControlledSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(), stdexec::set_stopped_t()>;
    Control* control;

    struct OnStop {
        Control* control;
        void operator()() const noexcept
        {
            CHECK(control->stop_calls.fetch_add(1) == 0);
            if (control->state) {
                ++control->state->stop_callbacks;
                control->state->all_stop_seen.count_down();
            }
            control->stop_seen.count_down();
            // Do not complete in the callback. Root-stop waits for request_stop
            // to return. Sibling cancellation may complete concurrently with
            // its tail: reset waits for this callback, and when_all retains its
            // source until the cancelling child/forwarder has arrived.
        }
    };

    template<class Receiver>
    struct Operation {
        Control* control;
        Receiver receiver;
        std::optional<stdexec::inplace_stop_callback<OnStop>> callback;
        bool started = false;

        ~Operation()
        {
            CHECK(!callback.has_value());
            if (started && control->state) {
                CHECK(control->state->live_operations.fetch_sub(1) > 0);
                ++control->state->operations_retired;
            }
        }

        void start() & noexcept
        {
            auto* local = control;
            CHECK(!started);
            started = true;
            if (local->state) {
                ++local->state->live_operations;
                ++local->state->operations_started;
            }
            callback.emplace(stdexec::get_stop_token(stdexec::get_env(receiver)), OnStop{local});
            local->operation = this;
            local->complete = [](void* pointer, bool stopped) noexcept {
                auto* self = static_cast<Operation*>(pointer);
                self->callback.reset();
                if (stopped)
                    stdexec::set_stopped(std::move(self->receiver));
                else
                    stdexec::set_value(std::move(self->receiver));
                // Terminal completion may destroy self; never access it here.
            };
            local->published.count_down();
            if (local->state)
                local->state->all_published.count_down();
            // Only independent Control storage is used after publication.
        }
    };

    template<class Receiver>
    auto connect(Receiver receiver) const
    {
        return Operation<Receiver>{control, std::move(receiver), {}, false};
    }
};

stdexec::task<int> branch_task(Guard guard, Control* gates, Outcome outcome)
{
    auto& state = *guard.state;
    auto& node = state.nodes[guard.index];
    CHECK(node.entered.fetch_add(1) == 0);
    state.all_entered.count_down();
    if (guard.index < state.depth) {
        if (state.topology == Topology::Chain) {
            // The pinned one-input when_all is the identity: pure task chain,
            // with no new stop source. Fork adds a source at every level.
            co_await stdexec::when_all(
                branch_task(Guard{state, guard.index + 1}, gates, outcome));
        } else {
            co_await stdexec::when_all(
                branch_task(Guard{state, guard.index + 1}, gates, outcome),
                branch_task(Guard{state, state.depth + 1 + guard.index}, gates, Outcome::Value));
        }
        CHECK(node.children.load() == 0);
        ++state.successful_joins;
    } else {
        const bool deepest = guard.index == state.depth;
        const int gate_index = deepest ? 0 : guard.index - state.depth;
        co_await ControlledSender{gates + gate_index};
        CHECK(outcome != Outcome::RootStop);
        if (is_error(outcome))
            throw std::runtime_error("expected deep-chain error");
        if (is_stopped(outcome)) {
            co_await stdexec::just_stopped();
            fail("a stopped leaf must not resume", __LINE__);
        }
    }
    ++state.business;
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
        CHECK(std::strcmp(caught.what(), "expected deep-chain error") == 0);
        return;
    }
    fail("expected runtime_error", __LINE__);
}

void run_case(int depth, Topology topology, Outcome outcome)
{
    Ownership state(depth, topology);
    auto gates = std::make_unique<Control[]>(state.leaf_count);
    for (int index = 0; index < state.leaf_count; ++index)
        gates[index].state = &state;
    stdexec::inplace_stop_source source;
    stdexec::run_loop scheduler;
    std::jthread scheduler_thread([&] { scheduler.run(); });
    Control outside_sibling;
    std::latch root_completed{1};
    bool has_value = false;
    bool received_error = false;
    bool wrapper_has_value = false;
    std::printf("begin depth=%d topology=%s outcome=%s nodes=%d leaves=%d\n",
                depth, name(topology), name(outcome), state.node_count, state.leaf_count);
    phase("starting");
    std::jthread runner([&] {
        if (outcome == Outcome::RootStop) {
            auto root = stdexec::starts_on(scheduler.get_scheduler(),
                branch_task(Guard{state, 0}, gates.get(), outcome));
            has_value = stdexec::sync_wait(stdexec::write_env(std::move(root),
                stdexec::prop{stdexec::get_stop_token, source.get_token()})).has_value();
            root_completed.count_down();
        } else {
            auto root = stdexec::let_error(
                stdexec::starts_on(scheduler.get_scheduler(),
                    optional_task(branch_task(Guard{state, 0}, gates.get(), outcome))),
                [&](std::exception_ptr error) {
                    check_expected_error(error);
                    received_error = true;
                    return stdexec::just(std::optional<int>{});
                }) | stdexec::then([&](std::optional<int> result) {
                    wrapper_has_value = result.has_value();
                    if (result)
                        CHECK(*result == 42);
                    // Document 9.3.1: inner frames must retire before another
                    // unrelated branch finishes, including local stopped.
                    state.check_retired();
                    root_completed.count_down();
                });
            has_value = stdexec::sync_wait(stdexec::when_all(
                std::move(root), ControlledSender{&outside_sibling})).has_value();
        }
    });

    state.all_entered.wait();
    state.all_published.wait();
    state.check_all_started();
    phase("all-started");
    if (outcome == Outcome::RootStop) {
        phase("stop-request");
        CHECK(!source.request_stop());
        state.all_stop_seen.wait();
        CHECK(state.stop_callbacks.load() == state.leaf_count);
        state.check_all_started();
        phase("stop-returned");
    }
    phase("leaf-completion");
    gates[0].finish(outcome == Outcome::RootStop);
    if (outcome == Outcome::Error || outcome == Outcome::Stopped) {
        phase("sibling-cancellation");
        // A parent group cannot finish until its direct side child does.
        // Acknowledge one level at a time, proving every nested source's stop
        // reaches its live sibling before moving to the next outer group.
        for (int index = state.leaf_count - 1; index > 0; --index) {
            gates[index].stop_seen.wait();
            // This side child still blocks its parent join. Cancellation is
            // not record retirement: stopped task frames retain their guards,
            // whereas an error releases the completed inner subtree before
            // the next outer when_all receives that error.
            CHECK(state.nodes[index - 1].live.load());
            CHECK(state.nodes[state.depth + index].live.load());
            CHECK(state.nodes[index - 1].children.load()
                  == (outcome == Outcome::Stopped ? 2 : 1));
            const int retired = outcome == Outcome::Stopped ? 0
                : 2 * (state.depth - index) + 1;
            CHECK(state.retired.load() == retired);
            CHECK(state.live.load() == state.node_count - retired);
            CHECK(!root_completed.try_wait());
            CHECK(state.successful_joins.load() == 0);
            if (index > 1)
                CHECK(gates[index - 1].stop_calls.load() == 0);
            gates[index].finish(true);
        }
    } else {
        // Fast variants retain the distinct, legal race where side branches
        // deliver value without accepting cancellation, stressing frame unwind.
        for (int index = 1; index < state.leaf_count; ++index)
            gates[index].finish(outcome == Outcome::RootStop);
    }
    root_completed.wait();
    phase("root-completed");
    if (outcome != Outcome::RootStop) {
        state.check_retired();
        CHECK(outside_sibling.finish_calls.load() == 0);
        outside_sibling.finish();
    }
    runner.join();
    // Finish only after root operation destruction, outstanding completion
    // delivery, and the external request_stop call have all completed.
    scheduler.finish();
    scheduler_thread.join();
    state.check_retired();
    CHECK(has_value == (outcome != Outcome::RootStop));
    CHECK(received_error == is_error(outcome));
    CHECK(wrapper_has_value == (outcome == Outcome::Value));
    CHECK(state.successful_joins.load() == (outcome == Outcome::Value ? depth : 0));
    const int expected_business = outcome == Outcome::Value ? state.node_count
        : is_fast(outcome) ? state.leaf_count - 1 : 0;
    CHECK(state.business.load() == expected_business);
    if (outcome == Outcome::Error || outcome == Outcome::Stopped)
        CHECK(state.stop_callbacks.load() == state.leaf_count - 1);
    for (int index = 0; index < state.leaf_count; ++index) {
        CHECK(gates[index].finish_calls.load() == 1);
        if (outcome == Outcome::RootStop ||
            (index != 0 && (outcome == Outcome::Error || outcome == Outcome::Stopped)))
            CHECK(gates[index].stop_calls.load() == 1);
    }
    std::printf("PASS depth=%d topology=%s outcome=%s nodes=%d retired=%d "
                "leaf_operations=%d callbacks=%d joins=%d business=%d live=0\n",
                depth, name(topology), name(outcome), state.node_count,
                state.retired.load(), state.operations_retired.load(),
                state.stop_callbacks.load(), state.successful_joins.load(), state.business.load());
    std::fflush(stdout);
}

int parse_depth(const char* value)
{
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    CHECK(errno == 0 && end != value && *end == '\0' && parsed >= 0 && parsed <= 16384);
    return static_cast<int>(parsed);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc == 1) {
        for (int depth : {0, 1, 8, 16}) {
            for (Topology topology : {Topology::Chain, Topology::Fork}) {
                for (Outcome outcome : {Outcome::Value, Outcome::Error,
                                        Outcome::Stopped, Outcome::RootStop,
                                        Outcome::ErrorFast, Outcome::StoppedFast})
                    run_case(depth, topology, outcome);
            }
        }
        std::printf("stdexec::task deep-chain dependency probe: 48 cases passed\n");
        return 0;
    }
    CHECK(argc == 7);
    CHECK(std::strcmp(argv[1], "--depth") == 0);
    CHECK(std::strcmp(argv[3], "--topology") == 0);
    CHECK(std::strcmp(argv[5], "--outcome") == 0);
    const int depth = parse_depth(argv[2]);
    Topology topology = Topology::Chain;
    if (std::strcmp(argv[4], "fork") == 0)
        topology = Topology::Fork;
    else
        CHECK(std::strcmp(argv[4], "chain") == 0);
    Outcome outcome = Outcome::Value;
    if (std::strcmp(argv[6], "error") == 0)
        outcome = Outcome::Error;
    else if (std::strcmp(argv[6], "stopped") == 0)
        outcome = Outcome::Stopped;
    else if (std::strcmp(argv[6], "root-stop") == 0)
        outcome = Outcome::RootStop;
    else if (std::strcmp(argv[6], "error-fast") == 0)
        outcome = Outcome::ErrorFast;
    else if (std::strcmp(argv[6], "stopped-fast") == 0)
        outcome = Outcome::StoppedFast;
    else
        CHECK(std::strcmp(argv[6], "value") == 0);
    run_case(depth, topology, outcome);
}
