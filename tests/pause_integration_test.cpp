#include "pause.hpp"

#include <stdexec/execution.hpp>

#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <expected>
#include <latch>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

// Real public Pause API tests. All coordinators are independent of the run_loop
// threads they release; no timing delays are used to establish correctness.
namespace {

const char* current_case = "initialization";
int current_iteration = 0;
std::uint64_t current_seed = 0;
std::uint64_t race_value_results = 0;
std::uint64_t race_stopped_results = 0;

[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed: case=%s iteration=%d seed=%" PRIu64 " line=%d: %s\n",
                 current_case, current_iteration, current_seed, line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

using pause::AdvanceMode;
using pause::GatePhase;
using pause::PauseBranch;
using pause::PauseEpoch;
using pause::PauseError;
using pause::PauseOrigin;
using pause::PauseSnapshot;
using pause::PauseSource;

template<class T>
concept CanRequestPause = requires(T&& source) {
    std::forward<T>(source).request_pause();
};
template<class T>
concept CanSnapshot = requires(T&& source) {
    std::forward<T>(source).snapshot();
};
template<class T>
concept CanSuspend = requires(T&& branch) {
    std::forward<T>(branch).suspend_point();
};
static_assert(!std::is_copy_constructible_v<PauseSource>);
static_assert(!std::is_move_constructible_v<PauseSource>);
static_assert(!std::is_copy_constructible_v<PauseBranch>);
static_assert(std::is_nothrow_move_constructible_v<PauseBranch>);
static_assert(!std::is_move_assignable_v<PauseBranch>);
static_assert(CanRequestPause<PauseSource&>);
static_assert(!CanRequestPause<const PauseSource&>);
static_assert(!CanRequestPause<PauseSource>);
static_assert(CanSnapshot<const PauseSource&>);
static_assert(!CanSnapshot<PauseSource>);
static_assert(CanSuspend<PauseBranch&>);
static_assert(!CanSuspend<PauseBranch>);

struct ChangeSignal {
    std::atomic<std::size_t> sequence{0};

    void signal() noexcept
    {
        sequence.fetch_add(1, std::memory_order_release);
        sequence.notify_all();
    }
    void wait(std::size_t observed) const noexcept
    {
        sequence.wait(observed, std::memory_order_acquire);
    }
};

void check_snapshot(const PauseSnapshot& snapshot)
{
    CHECK(snapshot.waiting_branch_count <= snapshot.live_branch_count);
    if (snapshot.phase == GatePhase::Open) {
        CHECK(!snapshot.paused_epoch && !snapshot.origin && !snapshot.step_round);
        CHECK(snapshot.waiting_branch_count == 0);
    } else {
        CHECK(snapshot.root_created && snapshot.origin.has_value());
        if (snapshot.phase == GatePhase::Collecting) {
            CHECK(!snapshot.paused_epoch);
            CHECK(snapshot.waiting_branch_count < snapshot.live_branch_count);
        } else {
            CHECK(snapshot.paused_epoch.has_value());
            CHECK(snapshot.paused_epoch->value <= snapshot.revision);
            CHECK(snapshot.waiting_branch_count == snapshot.live_branch_count);
        }
        if (snapshot.origin == PauseOrigin::SingleStep)
            CHECK(snapshot.step_round.has_value());
    }
}

template<class Predicate>
PauseSnapshot await_snapshot(ChangeSignal& changed, const PauseSource& source,
                             Predicate predicate)
{
    for (;;) {
        const auto observed = changed.sequence.load(std::memory_order_acquire);
        auto snapshot = source.snapshot();
        check_snapshot(snapshot);
        if (predicate(snapshot)) return snapshot;
        changed.wait(observed);
    }
}

PauseSnapshot await_paused(ChangeSignal& changed, const PauseSource& source)
{
    return await_snapshot(changed, source, [](const auto& snapshot) {
        return snapshot.phase == GatePhase::Paused;
    });
}

template<class T>
void expect_error(const std::expected<T, PauseError>& result, PauseError expected)
{
    CHECK(!result && result.error() == expected);
}

enum class Outcome { None, Value, Stopped, Error };

struct Host {
    stdexec::run_loop loop;
    stdexec::inplace_stop_source stop;
    ChangeSignal* changed = nullptr;
    std::atomic<Outcome> result{Outcome::None};
    std::atomic<int> completions{0};
    std::atomic<int> business{0};
    std::thread::id execution_thread;
    std::thread::id completion_thread;
    std::exception_ptr error;

    struct Receiver {
        using receiver_concept = stdexec::receiver_tag;
        Host* host;

        auto get_env() const noexcept
        {
            return stdexec::env{
                stdexec::prop{stdexec::get_stop_token, host->stop.get_token()},
                stdexec::prop{stdexec::get_start_scheduler, host->loop.get_scheduler()}};
        }
        void finish(Outcome result) noexcept
        {
            auto* local = host;
            CHECK(local->completions.fetch_add(1) == 0);
            local->completion_thread = std::this_thread::get_id();
            local->result.store(result, std::memory_order_release);
            local->loop.finish();
            if (local->changed) local->changed->signal();
        }
        void set_value() noexcept { finish(Outcome::Value); }
        void set_stopped() noexcept { finish(Outcome::Stopped); }
        void set_error(std::exception_ptr error) noexcept
        {
            host->error = error;
            finish(Outcome::Error);
        }
    };

    template<class Sender>
    void run(Sender sender)
    {
        execution_thread = std::this_thread::get_id();
        auto operation = stdexec::connect(
            stdexec::starts_on(loop.get_scheduler(), std::move(sender)), Receiver{this});
        stdexec::start(operation);
        loop.run();
    }

    void check(Outcome expected, bool require_affinity = true) const
    {
        CHECK(result.load() == expected && completions.load() == 1);
        if (require_affinity) CHECK(completion_thread == execution_thread);
    }
};

int drive_single_step(ChangeSignal& changed, PauseSource& source, Host& host,
                      int maximum_batches, std::size_t expected_width = 0)
{
    int batches = 0;
    while (host.result.load(std::memory_order_acquire) == Outcome::None) {
        const auto observed = changed.sequence.load(std::memory_order_acquire);
        const auto snapshot = source.snapshot();
        check_snapshot(snapshot);
        if (snapshot.phase == GatePhase::Paused && snapshot.waiting_branch_count != 0) {
            CHECK(expected_width == 0 || snapshot.waiting_branch_count == expected_width);
            CHECK(source.resume(*snapshot.paused_epoch, AdvanceMode::SingleStep));
            CHECK(++batches <= maximum_batches);
        } else if (host.result.load(std::memory_order_acquire) == Outcome::None) {
            changed.wait(observed);
        }
    }
    return batches;
}

stdexec::task<void> points(PauseBranch& branch, Host& host, int count = 1)
{
    for (int point = 0; point != count; ++point) {
        CHECK(std::this_thread::get_id() == host.execution_thread);
        co_await branch.suspend_point({static_cast<std::size_t>(point)});
        CHECK(std::this_thread::get_id() == host.execution_thread);
        ++host.business;
    }
}

stdexec::task<void> owned_point(PauseBranch branch, Host& host)
{
    co_await branch.suspend_point();
    CHECK(std::this_thread::get_id() == host.execution_thread);
    ++host.business;
}

void test_api_and_empty_gate()
{
    ChangeSignal changed;
    PauseSource source{AdvanceMode::Continuous, [&]() noexcept { changed.signal(); }};
    auto initial = source.snapshot();
    check_snapshot(initial);
    CHECK(initial.revision == 0 && !initial.root_created && initial.live_branch_count == 0);
    expect_error(source.make_branch(), PauseError::RootNotCreated);
    expect_error(source.request_pause(), PauseError::RootNotCreated);
    expect_error(source.resume(PauseEpoch{0}, AdvanceMode::Continuous), PauseError::RootNotCreated);
    {
        auto result = source.make_root_branch();
        CHECK(result);
        PauseBranch root{std::move(*result)};
        CHECK(source.snapshot().revision == initial.revision + 1);
        expect_error(source.make_root_branch(), PauseError::RootAlreadyCreated);
        expect_error(source.resume(PauseEpoch{0}, AdvanceMode::Continuous), PauseError::NotPaused);
        expect_error(root.fork_and_block_self<0>(), PauseError::InvalidForkCount);
        expect_error(root.fork_and_block_self(0), PauseError::InvalidForkCount);
        expect_error(result->fork_and_block_self(1), PauseError::InvalidBranchState);
        {
            auto independent = source.make_branch();
            CHECK(independent && source.snapshot().live_branch_count == 2);
        }
        CHECK(source.snapshot().live_branch_count == 1);
        CHECK(source.request_pause());
        const auto collecting = source.snapshot();
        CHECK(collecting.phase == GatePhase::Collecting);
        expect_error(source.request_pause(), PauseError::PauseAlreadyCollecting);
        expect_error(source.make_branch(), PauseError::InvalidBranchState);
        CHECK(source.snapshot().revision == collecting.revision);
    }
    auto empty = source.snapshot();
    check_snapshot(empty);
    CHECK(empty.phase == GatePhase::Paused && empty.live_branch_count == 0);
    expect_error(source.request_pause(), PauseError::AlreadyPaused);
    expect_error(source.resume(PauseEpoch{empty.paused_epoch->value + 1}, AdvanceMode::Continuous),
                 PauseError::StalePauseEpoch);
    CHECK(source.resume(*empty.paused_epoch, AdvanceMode::SingleStep));
    auto next = source.snapshot();
    CHECK(next.phase == GatePhase::Paused && next.live_branch_count == 0);
    CHECK(next.step_round == 1 && next.paused_epoch != empty.paused_epoch);
    expect_error(source.resume(*empty.paused_epoch, AdvanceMode::Continuous), PauseError::StalePauseEpoch);
    CHECK(source.resume(*next.paused_epoch, AdvanceMode::Continuous));
    CHECK(source.snapshot().phase == GatePhase::Open);
    CHECK(source.request_pause());
    empty = source.snapshot();
    CHECK(empty.phase == GatePhase::Paused && empty.live_branch_count == 0);
    CHECK(source.resume(*empty.paused_epoch, AdvanceMode::Continuous));
    check_snapshot(source.snapshot());
}

void test_open_and_single_step(bool single_step)
{
    ChangeSignal changed;
    PauseSource source{single_step ? AdvanceMode::SingleStep : AdvanceMode::Continuous,
                       [&]() noexcept { changed.signal(); }};
    auto root = source.make_root_branch();
    CHECK(root);
    Host host;
    std::jthread runner([&] { host.run(points(*root, host, 4)); });
    if (single_step) {
        std::optional<PauseEpoch> previous;
        for (int round = 0; round != 4; ++round) {
            const auto snapshot = await_snapshot(changed, source, [&](const auto& state) {
                return state.phase == GatePhase::Paused && state.paused_epoch != previous;
            });
            CHECK(snapshot.step_round == static_cast<std::size_t>(round));
            CHECK(snapshot.origin == PauseOrigin::SingleStep);
            CHECK(snapshot.waiting_branch_count == 1 && host.business.load() == round);
            previous = snapshot.paused_epoch;
            CHECK(source.resume(*snapshot.paused_epoch,
                                round == 3 ? AdvanceMode::Continuous : AdvanceMode::SingleStep));
        }
    }
    runner.join();
    host.check(Outcome::Value);
    CHECK(host.business.load() == 4);
    auto snapshot = source.snapshot();
    check_snapshot(snapshot);
    CHECK(snapshot.phase == GatePhase::Open && snapshot.live_branch_count == 1);
}

void test_all_arrive_or_retire()
{
    ChangeSignal changed;
    PauseSource source{AdvanceMode::Continuous, [&]() noexcept { changed.signal(); }};
    auto root = source.make_root_branch();
    CHECK(root);
    Host host;
    std::jthread runner;
    {
        auto retiring = source.make_branch();
        CHECK(retiring && source.request_pause());
        runner = std::jthread([&] { host.run(points(*root, host)); });
        const auto partial = await_snapshot(changed, source, [](const auto& snapshot) {
            return snapshot.waiting_branch_count == 1;
        });
        CHECK(partial.phase == GatePhase::Collecting && partial.live_branch_count == 2);
    }
    const auto paused = source.snapshot();
    CHECK(paused.phase == GatePhase::Paused && paused.waiting_branch_count == 1);
    CHECK(source.resume(*paused.paused_epoch, AdvanceMode::Continuous));
    runner.join();
    host.check(Outcome::Value);
    CHECK(host.business.load() == 1);
}

void test_parent_reentry_and_epoch()
{
    ChangeSignal changed;
    PauseSource source{AdvanceMode::Continuous, [&]() noexcept { changed.signal(); }};
    auto root = source.make_root_branch();
    CHECK(root);
    {
        auto child = root->fork_and_block_self<1>();
        CHECK(child);
        expect_error(root->fork_and_block_self(1), PauseError::InvalidBranchState);
        CHECK(source.request_pause());
        CHECK(source.snapshot().phase == GatePhase::Collecting);
    }
    auto empty = source.snapshot();
    CHECK(empty.phase == GatePhase::Paused && empty.live_branch_count == 0);
    CHECK(source.resume(*empty.paused_epoch, AdvanceMode::SingleStep));
    empty = source.snapshot();
    CHECK(empty.phase == GatePhase::Paused && empty.step_round == 1);
    Host host;
    std::jthread runner([&] { host.run(points(*root, host)); });
    auto joined = await_snapshot(changed, source, [](const auto& snapshot) {
        return snapshot.waiting_branch_count == 1;
    });
    CHECK(joined.phase == GatePhase::Paused && joined.paused_epoch == empty.paused_epoch);
    CHECK(joined.revision > empty.revision && joined.live_branch_count == 1);
    CHECK(source.resume(*joined.paused_epoch, AdvanceMode::Continuous));
    runner.join();
    host.check(Outcome::Value);
    CHECK(host.business.load() == 1);
    // A second non-consuming fork proves that the parent can be reused after
    // the previous complete operation and after all direct children retired.
    {
        auto children = root->fork_and_block_self(3);
        CHECK(children && children->size() == 3);
        CHECK(source.snapshot().live_branch_count == 3);
    }
    CHECK(source.snapshot().live_branch_count == 0);
    CHECK(source.snapshot().phase == GatePhase::Open);
}

void test_two_resources(bool cancel_first)
{
    ChangeSignal changed;
    PauseSource source{AdvanceMode::Continuous, [&]() noexcept { changed.signal(); }};
    auto first = source.make_root_branch();
    auto second = source.make_branch();
    CHECK(first && second && source.request_pause());
    Host a, b;
    std::jthread runner_a([&] { a.run(points(*first, a)); });
    std::jthread runner_b([&] { b.run(points(*second, b)); });
    const auto paused = await_paused(changed, source);
    CHECK(paused.waiting_branch_count == 2);
    if (cancel_first) {
        CHECK(!a.stop.request_stop());
        const auto cancelled = source.snapshot();
        CHECK(cancelled.phase == GatePhase::Paused && cancelled.waiting_branch_count == 1);
        CHECK(cancelled.live_branch_count == 1 && cancelled.paused_epoch == paused.paused_epoch);
        CHECK(cancelled.revision > paused.revision);
        CHECK(!b.stop.stop_requested());
    }
    CHECK(source.resume(*paused.paused_epoch, AdvanceMode::Continuous));
    runner_a.join();
    runner_b.join();
    a.check(cancel_first ? Outcome::Stopped : Outcome::Value);
    b.check(Outcome::Value);
    CHECK(a.business.load() == (cancel_first ? 0 : 1) && b.business.load() == 1);
    CHECK(a.execution_thread != b.execution_thread);
    CHECK(source.snapshot().live_branch_count == (cancel_first ? 1u : 2u));
}

struct ExternalControl {
    std::latch published{1};
    void* operation = nullptr;
    void (*complete)(void*, Outcome) noexcept = nullptr;

    void finish(Outcome outcome = Outcome::Value) noexcept
    {
        published.wait();
        complete(operation, outcome);
    }
};

struct ExternalSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(), stdexec::set_stopped_t(), stdexec::set_error_t(std::exception_ptr)>;
    ExternalControl* control;

    template<class Receiver>
    struct Operation {
        ExternalControl* control;
        Receiver receiver;
        void start() & noexcept
        {
            auto* local = control;
            local->operation = this;
            local->complete = [](void* pointer, Outcome outcome) noexcept {
                auto* self = static_cast<Operation*>(pointer);
                if (outcome == Outcome::Value) stdexec::set_value(std::move(self->receiver));
                else if (outcome == Outcome::Stopped) stdexec::set_stopped(std::move(self->receiver));
                else stdexec::set_error(std::move(self->receiver),
                    std::make_exception_ptr(std::runtime_error("expected external business error")));
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

stdexec::task<int> external_child(PauseBranch branch, ExternalControl& external,
                                 std::atomic<int>& business)
{
    co_await branch.suspend_point();
    co_await ExternalSender{&external};
    co_await branch.suspend_point();
    ++business;
    co_return 42;
}

stdexec::task<std::optional<int>> as_optional_task(stdexec::task<int> inner)
{
    auto result = co_await stdexec::stopped_as_optional(std::move(inner));
    co_return result;
}

stdexec::task<void> external_pair(PauseBranch parent, Host& host,
                                  ExternalControl& a, ExternalControl& b,
                                  bool convert_stopped, bool expect_empty)
{
    co_await parent.suspend_point();
    auto children = parent.fork_and_block_self<2>();
    CHECK(children);
    if (convert_stopped) {
        auto [result, ignored] = co_await stdexec::when_all(
            as_optional_task(external_child(std::move((*children)[0]), a, host.business)),
            external_child(std::move((*children)[1]), b, host.business));
        CHECK(result.has_value() != expect_empty && ignored == 42);
        if (result) CHECK(*result == 42);
    } else {
        co_await stdexec::when_all(
            external_child(std::move((*children)[0]), a, host.business),
            external_child(std::move((*children)[1]), b, host.business));
    }
    co_await parent.suspend_point();
    ++host.business;
}

void test_external_completion(Outcome outcome, bool convert_stopped)
{
    ChangeSignal changed;
    PauseSource source{AdvanceMode::Continuous, [&]() noexcept { changed.signal(); }};
    auto root = source.make_root_branch();
    CHECK(root);
    Host host;
    ExternalControl a, b;
    std::jthread runner([&] {
        host.run(external_pair(std::move(*root), host, a, b, convert_stopped,
                               outcome == Outcome::Stopped));
    });
    a.published.wait();
    b.published.wait();
    CHECK(source.request_pause());
    b.finish();
    const auto partial = await_snapshot(changed, source, [](const auto& snapshot) {
        return snapshot.waiting_branch_count == 1;
    });
    CHECK(partial.phase == GatePhase::Collecting && partial.live_branch_count == 2);
    a.finish(outcome);
    if (convert_stopped && outcome != Outcome::Error) {
        // The optional-task host retires A before publishing value to when_all.
        // B can now converge without waiting for the join to release A's frame.
        const auto paused = await_paused(changed, source);
        const auto expected = outcome == Outcome::Stopped ? 1u : 2u;
        CHECK(paused.live_branch_count == expected && paused.waiting_branch_count == expected);
        CHECK(source.resume(*paused.paused_epoch, AdvanceMode::Continuous));
    }
    runner.join();
    host.check(convert_stopped && outcome != Outcome::Error ? Outcome::Value : outcome);
    CHECK(host.business.load() == (convert_stopped && outcome != Outcome::Error
                                   ? (outcome == Outcome::Value ? 3 : 2) : 0));
    CHECK(source.snapshot().live_branch_count == 0);
    CHECK(source.snapshot().waiting_branch_count == 0);
    if (outcome == Outcome::Error) {
        CHECK(host.error);
        bool expected = false;
        try { std::rethrow_exception(host.error); }
        catch (const std::runtime_error& error) {
            expected = std::strcmp(error.what(), "expected external business error") == 0;
        }
        CHECK(expected);
    }
}

stdexec::task<void> retained_pair(PauseBranch parent, Host& host,
                                 ExternalControl& a, ExternalControl& b,
                                 std::latch& converted)
{
    co_await parent.suspend_point();
    auto children = parent.fork_and_block_self<2>();
    CHECK(children);
    co_await stdexec::when_all(
        stdexec::stopped_as_optional(
            external_child(std::move((*children)[0]), a, host.business))
            | stdexec::then([&](std::optional<int> result) {
                  CHECK(!result);
                  converted.count_down();
              }),
        external_child(std::move((*children)[1]), b, host.business));
    fail("retained counterexample must exit by host cancellation", __LINE__);
}

void test_retained_stopped_counterexample()
{
    ChangeSignal changed;
    PauseSource source{AdvanceMode::Continuous, [&]() noexcept { changed.signal(); }};
    auto root = source.make_root_branch();
    CHECK(root);
    Host host;
    ExternalControl a, b;
    std::latch converted{1};
    std::jthread runner([&] {
        host.run(retained_pair(std::move(*root), host, a, b, converted));
    });
    a.published.wait();
    b.published.wait();
    CHECK(source.request_pause());
    b.finish();
    await_snapshot(changed, source, [](const auto& snapshot) {
        return snapshot.waiting_branch_count == 1;
    });
    a.finish(Outcome::Stopped);
    converted.wait();
    const auto retained = source.snapshot();
    CHECK(retained.phase == GatePhase::Collecting);
    CHECK(retained.live_branch_count == 2 && retained.waiting_branch_count == 1);
    CHECK(host.result.load() == Outcome::None && host.business.load() == 0);
    // Root cancellation safely completes B and releases the entire join. This
    // deliberately retained frame is never destroyed while its operation runs.
    CHECK(!host.stop.request_stop());
    runner.join();
    host.check(Outcome::Stopped, false);
    CHECK(source.snapshot().live_branch_count == 0);
    CHECK(source.snapshot().waiting_branch_count == 0);
}

struct TreeState {
    std::atomic<int> entries{0};
    std::atomic<int> joins{0};
    std::atomic<int> next_leaf{0};
    std::array<ExternalControl, 8> leaves;
    bool hold_leaves = false;
    Host* host = nullptr;
};

stdexec::task<void> tree(PauseBranch branch, TreeState& state, int depth)
{
    co_await branch.suspend_point();
    CHECK(std::this_thread::get_id() == state.host->execution_thread);
    ++state.entries;
    if (depth == 0) {
        if (state.hold_leaves) {
            const int index = state.next_leaf.fetch_add(1);
            CHECK(index < static_cast<int>(state.leaves.size()));
            co_await ExternalSender{&state.leaves[index]};
            co_await branch.suspend_point();
            CHECK(std::this_thread::get_id() == state.host->execution_thread);
        }
        co_return;
    }
    auto children = branch.fork_and_block_self<2>();
    CHECK(children);
    co_await stdexec::when_all(tree(std::move((*children)[0]), state, depth - 1),
                             tree(std::move((*children)[1]), state, depth - 1));
    co_await branch.suspend_point();
    CHECK(std::this_thread::get_id() == state.host->execution_thread);
    ++state.joins;
}

void test_recursive_fork(bool single_step, bool cancel)
{
    ChangeSignal changed;
    PauseSource source{single_step ? AdvanceMode::SingleStep : AdvanceMode::Continuous,
                       [&]() noexcept { changed.signal(); }};
    auto root = source.make_root_branch();
    CHECK(root);
    TreeState state;
    state.hold_leaves = cancel;
    Host host;
    state.host = &host;
    host.changed = &changed;
    std::jthread runner([&] { host.run(tree(std::move(*root), state, 3)); });
    if (cancel) {
        for (auto& leaf : state.leaves) leaf.published.wait();
        CHECK(source.snapshot().live_branch_count == 8);
        CHECK(source.request_pause());
        for (auto& leaf : state.leaves) leaf.finish();
        const auto paused = await_paused(changed, source);
        CHECK(paused.waiting_branch_count == 8);
        CHECK(!host.stop.request_stop());
    } else if (single_step) {
        const int batches = drive_single_step(changed, source, host, 22);
        CHECK(batches >= 4);
    }
    runner.join();
    // Pinned when_all temporarily adds a barrier participant in its external
    // stop callback. Its final arrival can consequently deliver stopped on the
    // request_stop thread after every child's own affine hop has completed.
    // This outer-combinator completion is outside Pause's direct-await promise.
    host.check(cancel ? Outcome::Stopped : Outcome::Value, !cancel);
    CHECK(state.entries.load() == 15);
    CHECK(state.joins.load() == (cancel ? 0 : 7));
    CHECK(source.snapshot().live_branch_count == 0);
    CHECK(source.snapshot().waiting_branch_count == 0);
    check_snapshot(source.snapshot());
}

stdexec::task<void> across_sources(PauseBranch first, PauseBranch second, Host& host)
{
    co_await first.suspend_point();
    CHECK(std::this_thread::get_id() == host.execution_thread);
    ++host.business;
    co_await second.suspend_point();
    CHECK(std::this_thread::get_id() == host.execution_thread);
    ++host.business;
}

void test_cross_source_task()
{
    ChangeSignal changed;
    PauseSource first{AdvanceMode::SingleStep, [&]() noexcept { changed.signal(); }};
    PauseSource second{AdvanceMode::SingleStep, [&]() noexcept { changed.signal(); }};
    auto a = first.make_root_branch();
    auto b = second.make_root_branch();
    CHECK(a && b);
    Host host;
    std::jthread runner([&] {
        host.run(across_sources(std::move(*a), std::move(*b), host));
    });
    auto paused = await_paused(changed, first);
    CHECK(second.snapshot().phase == GatePhase::Collecting);
    CHECK(first.resume(*paused.paused_epoch, AdvanceMode::Continuous));
    paused = await_paused(changed, second);
    CHECK(host.business.load() == 1);
    CHECK(second.resume(*paused.paused_epoch, AdvanceMode::Continuous));
    runner.join();
    host.check(Outcome::Value);
    CHECK(host.business.load() == 2);
    CHECK(first.snapshot().live_branch_count == 0 && second.snapshot().live_branch_count == 0);
}

void test_resume_stop_controller_race()
{
    constexpr int count = 4;
    ChangeSignal changed;
    PauseSource source{AdvanceMode::Continuous, [&]() noexcept { changed.signal(); }};
    auto root = source.make_root_branch();
    CHECK(root);
    auto branches = root->fork_and_block_self<count>();
    CHECK(branches && source.request_pause());
    std::array<Host, count> hosts;
    std::array<std::jthread, count> runners;
    for (int index = 0; index != count; ++index) {
        runners[index] = std::jthread([&, index] {
            hosts[index].run(owned_point(std::move((*branches)[index]), hosts[index]));
        });
    }
    const auto paused = await_paused(changed, source);
    CHECK(paused.waiting_branch_count == count);
    std::atomic<bool> observer_done{false};
    std::jthread observer([&] {
        std::size_t revision = 0;
        while (!observer_done.load(std::memory_order_acquire)) {
            const auto observed = changed.sequence.load(std::memory_order_acquire);
            auto snapshot = source.snapshot();
            check_snapshot(snapshot);
            CHECK(snapshot.revision >= revision);
            revision = snapshot.revision;
            if (!observer_done.load(std::memory_order_acquire)) changed.wait(observed);
        }
    });
    std::barrier start{2 * count + 1};
    std::atomic<int> successful_resume{0};
    std::array<std::jthread, count> controllers, stoppers;
    for (int index = 0; index != count; ++index) {
        controllers[index] = std::jthread([&] {
            start.arrive_and_wait();
            auto result = source.resume(*paused.paused_epoch, AdvanceMode::SingleStep);
            if (result) ++successful_resume;
            else CHECK(result.error() == PauseError::NotPaused
                       || result.error() == PauseError::StalePauseEpoch);
        });
        stoppers[index] = std::jthread([&, index] {
            start.arrive_and_wait();
            CHECK(!hosts[index].stop.request_stop());
        });
    }
    start.arrive_and_wait();
    for (auto& thread : controllers) thread.join();
    for (auto& thread : stoppers) thread.join();
    for (auto& thread : runners) thread.join();
    observer_done.store(true, std::memory_order_release);
    changed.signal();
    observer.join();
    CHECK(successful_resume.load() == 1);
    for (const auto& host : hosts) {
        const auto result = host.result.load();
        CHECK(result == Outcome::Value || result == Outcome::Stopped);
        host.check(result);
        CHECK(host.business.load() == (result == Outcome::Value ? 1 : 0));
        if (result == Outcome::Value) ++race_value_results;
        else ++race_stopped_results;
    }
    const auto final = source.snapshot();
    check_snapshot(final);
    CHECK(final.phase == GatePhase::Paused && final.live_branch_count == 0);
    CHECK(final.waiting_branch_count == 0 && final.step_round == 1);
    CHECK(final.paused_epoch != paused.paused_epoch);
}

struct Random {
    std::uint64_t state;

    std::uint64_t next() noexcept
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    }
};

void yield_times(int count) noexcept
{
    for (int index = 0; index != count; ++index) std::this_thread::yield();
}

void test_random_fork_pause(Random& random)
{
    const int count = 1 + static_cast<int>(random.next() % 4);
    const int fork_yields = static_cast<int>(random.next() % 8);
    const int pause_yields = static_cast<int>(random.next() % 8);
    const auto cancelled = random.next();
    const auto mode = random.next() % 2 ? AdvanceMode::Continuous : AdvanceMode::SingleStep;
    ChangeSignal changed;
    PauseSource source{AdvanceMode::Continuous, [&]() noexcept { changed.signal(); }};
    auto root = source.make_root_branch();
    CHECK(root);
    std::barrier start{2};
    std::jthread controller([&] {
        start.arrive_and_wait();
        yield_times(pause_yields);
        CHECK(source.request_pause());
    });
    start.arrive_and_wait();
    yield_times(fork_yields);
    auto children = root->fork_and_block_self(static_cast<std::size_t>(count));
    CHECK(children);
    controller.join();
    const auto collecting = source.snapshot();
    CHECK(collecting.phase == GatePhase::Collecting);
    CHECK(collecting.live_branch_count == static_cast<std::size_t>(count));
    std::array<Host, 4> hosts;
    std::array<std::jthread, 4> runners;
    for (int index = 0; index != count; ++index) {
        if ((cancelled >> index) & 1) CHECK(!hosts[index].stop.request_stop());
        runners[index] = std::jthread([&, index] {
            hosts[index].run(owned_point(std::move((*children)[index]), hosts[index]));
        });
    }
    const auto paused = await_paused(changed, source);
    CHECK(source.resume(*paused.paused_epoch, mode));
    for (int index = 0; index != count; ++index) {
        runners[index].join();
        hosts[index].check((cancelled >> index) & 1 ? Outcome::Stopped : Outcome::Value);
    }
    const auto final = source.snapshot();
    check_snapshot(final);
    CHECK(final.live_branch_count == 0 && final.waiting_branch_count == 0);
    CHECK(final.phase == (mode == AdvanceMode::Continuous ? GatePhase::Open : GatePhase::Paused));
}

void test_random_reentry_resume(Random& random)
{
    const int enter_yields = static_cast<int>(random.next() % 8);
    const int resume_yields = static_cast<int>(random.next() % 8);
    const auto mode = random.next() % 2 ? AdvanceMode::Continuous : AdvanceMode::SingleStep;
    ChangeSignal changed;
    PauseSource source{AdvanceMode::Continuous, [&]() noexcept { changed.signal(); }};
    auto root = source.make_root_branch();
    CHECK(root);
    {
        auto child = root->fork_and_block_self<1>();
        CHECK(child);
    }
    CHECK(source.request_pause());
    const auto empty = source.snapshot();
    CHECK(empty.phase == GatePhase::Paused && empty.live_branch_count == 0);
    Host host;
    host.changed = &changed;
    std::barrier start{3};
    std::jthread runner([&] {
        start.arrive_and_wait();
        yield_times(enter_yields);
        host.run(owned_point(std::move(*root), host));
    });
    std::jthread controller([&] {
        start.arrive_and_wait();
        yield_times(resume_yields);
        CHECK(source.resume(*empty.paused_epoch, mode));
    });
    start.arrive_and_wait();
    controller.join();
    // SingleStep may have established the new empty epoch before the parent
    // reentered. In that case it legitimately waits in the new round.
    while (host.result.load(std::memory_order_acquire) == Outcome::None) {
        const auto observed = changed.sequence.load(std::memory_order_acquire);
        const auto snapshot = source.snapshot();
        check_snapshot(snapshot);
        if (snapshot.phase == GatePhase::Paused && snapshot.waiting_branch_count != 0) {
            CHECK(mode == AdvanceMode::SingleStep);
            CHECK(snapshot.paused_epoch != empty.paused_epoch);
            CHECK(source.resume(*snapshot.paused_epoch, AdvanceMode::Continuous));
        } else if (host.result.load(std::memory_order_acquire) == Outcome::None) {
            changed.wait(observed);
        }
    }
    runner.join();
    host.check(Outcome::Value);
    CHECK(host.business.load() == 1 && source.snapshot().live_branch_count == 0);
    check_snapshot(source.snapshot());
}

stdexec::task<void> deep_chain(PauseBranch branch, Host& host, int depth)
{
    co_await branch.suspend_point();
    CHECK(std::this_thread::get_id() == host.execution_thread);
    ++host.business;
    if (depth != 0) {
        auto child = branch.fork_and_block_self<1>();
        CHECK(child);
        co_await deep_chain(std::move((*child)[0]), host, depth - 1);
        co_await branch.suspend_point();
        CHECK(std::this_thread::get_id() == host.execution_thread);
        ++host.business;
    }
}

void test_deep_chain(int depth, bool single_step)
{
    ChangeSignal changed;
    PauseSource source{single_step ? AdvanceMode::SingleStep : AdvanceMode::Continuous,
                       [&]() noexcept { changed.signal(); }};
    auto root = source.make_root_branch();
    CHECK(root);
    Host host;
    host.changed = &changed;
    std::jthread runner([&] { host.run(deep_chain(std::move(*root), host, depth)); });
    int batches = 0;
    if (single_step) batches = drive_single_step(changed, source, host, 2 * depth + 1, 1);
    runner.join();
    host.check(Outcome::Value);
    CHECK(host.business.load() == 2 * depth + 1);
    CHECK(!single_step || batches == 2 * depth + 1);
    CHECK(source.snapshot().live_branch_count == 0);
    check_snapshot(source.snapshot());
}

} // namespace

int main(int argc, char** argv)
{
    int iterations = 100;
    std::uint64_t seed = UINT64_C(0x3f09b7a561d82c45);
    CHECK(argc % 2 == 1);
    for (int index = 1; index < argc; index += 2) {
        char* end = nullptr;
        errno = 0;
        const auto parsed = std::strtoull(argv[index + 1], &end, 0);
        CHECK(end != argv[index + 1] && *end == '\0' && errno != ERANGE && parsed != 0);
        if (std::strcmp(argv[index], "--iterations") == 0) {
            CHECK(parsed <= 1000000);
            iterations = static_cast<int>(parsed);
        } else {
            CHECK(std::strcmp(argv[index], "--seed") == 0);
            seed = parsed;
        }
    }
    Random random{seed};
    current_seed = seed;
    current_case = "deep_chain";
    for (int depth : {32, 64}) {
        test_deep_chain(depth, false);
        test_deep_chain(depth, true);
    }
    for (int iteration = 0; iteration != iterations; ++iteration) {
        current_iteration = iteration;
        const auto run_case = [](const char* name, auto test) {
            current_case = name;
            test();
        };
        run_case("api_empty", test_api_and_empty_gate);
        run_case("continuous", [] { test_open_and_single_step(false); });
        run_case("single_step", [] { test_open_and_single_step(true); });
        run_case("arrive_or_retire", test_all_arrive_or_retire);
        run_case("parent_epoch", test_parent_reentry_and_epoch);
        run_case("two_resources", [] { test_two_resources(false); });
        run_case("independent_cancel", [] { test_two_resources(true); });
        run_case("external_stopped", [] { test_external_completion(Outcome::Stopped, false); });
        run_case("external_error", [] { test_external_completion(Outcome::Error, false); });
        run_case("external_optional", [] { test_external_completion(Outcome::Stopped, true); });
        run_case("optional_value", [] { test_external_completion(Outcome::Value, true); });
        run_case("optional_error", [] { test_external_completion(Outcome::Error, true); });
        run_case("retained_stopped", test_retained_stopped_counterexample);
        run_case("tree_continuous", [] { test_recursive_fork(false, false); });
        run_case("tree_single_step", [] { test_recursive_fork(true, false); });
        run_case("tree_cancel", [] { test_recursive_fork(false, true); });
        run_case("cross_source", test_cross_source_task);
        run_case("resume_stop_race", test_resume_stop_controller_race);
        run_case("random_fork_pause", [&] { test_random_fork_pause(random); });
        run_case("random_reentry_resume", [&] { test_random_reentry_resume(random); });
    }
    std::printf("real Pause public integration: %d cases passed (%d repetitions, 20 cases/round + 4 deep chains)\n",
                iterations * 20 + 4, iterations);
    std::printf("resume/stop contention: %d rounds, 4 run_loops + 4 controllers + 4 stoppers + observer\n",
                iterations);
    std::printf("contended completions: value=%" PRIu64 " stopped=%" PRIu64 " (exactly once)\n",
                race_value_results, race_stopped_results);
    std::puts("API/errors/epochs/empty barriers; 4-step control; independent cancellation/resources;");
    std::puts("15-node recursive forks/join/reentry/cancel; external when_all error/stopped/optional host;");
    std::puts("cross-Source task and monotonic observer snapshots; latch/barrier/atomic notification only.");
    std::printf("random races: %d fork/pause and %d parent/reentry rounds, seed=%" PRIu64 "\n",
                iterations, iterations, seed);
    std::puts("real 32/64-level chains passed Continuous and SingleStep (65/129 pause points each).");
}
