#define PAUSE_ENABLE_INDEPENDENT_BRANCH 1
// Real Pause integration with native run_loop and static_thread_pool schedulers.
// One-worker coverage ensures that pause/controller waits never block a pool worker.
// No assertion assumes that a multi-worker scheduler resumes on the same OS thread.
#include "../support/pause_test_notifications.hpp"
#include "../../examples/common/example_support.hpp"
#include <exec/static_thread_pool.hpp>

#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <string_view>
#include <thread>

namespace {
using namespace pause_examples;
const char* current_case = "setup";
const bool trace_cases = std::getenv("PAUSE_SCHEDULER_TRACE") != nullptr;
void set_case(const char* name) {
    current_case = name;
    if (trace_cases) { std::printf("case=%s\n", name); std::fflush(stdout); }
}

[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK case=%s line=%d: %s\n", current_case, line, expression);
    std::_Exit(90);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

struct BusinessError {};
struct ControllerError {};

struct State {
    ChangeSignal changed;
    PauseSource source;
    std::atomic<int> visits{0}, frames{0}, started{0}, finished{0}, joins{0};
    std::atomic<bool> first_stopped{false};
    const std::thread::id caller = std::this_thread::get_id();
    std::thread::id only_worker;
    int result = 0;

    explicit State(AdvanceMode mode = AdvanceMode::SingleStep)
        : source(mode, pause_test::notifications([this]() noexcept { changed.signal(); })) {}
    void drained() const {
        require_drained(source);
        CHECK(frames == 0);
    }
};

struct Frame {
    State& state;
    explicit Frame(State& s) : state(s) { ++state.frames; }
    ~Frame() { --state.frames; }
};

stdexec::task<int> leaf(PauseBranch branch, State& state, int index, bool error = false)
{
    Frame frame{state};
    ++state.started;
    for (int stage = 0; stage != 3; ++stage) {
        co_await branch.suspend_point({static_cast<std::size_t>(stage)});
        CHECK(std::this_thread::get_id() != state.caller);
        if (state.only_worker != std::thread::id{})
            CHECK(std::this_thread::get_id() == state.only_worker);
        if (error && index == 0) throw BusinessError{};
        ++state.visits;
    }
    ++state.finished;
    co_return index + 1;
}

stdexec::task<void> tree(PauseBranch parent, State& state, bool error = false)
{
    auto children = take(parent.fork_and_block_self<4>());
    auto [a, b, c, d] = co_await stdexec::when_all(
        leaf(std::move(children[0]), state, 0, error),
        leaf(std::move(children[1]), state, 1),
        leaf(std::move(children[2]), state, 2),
        leaf(std::move(children[3]), state, 3));
    co_await parent.suspend_point();
    state.result = a + b + c + d;
    ++state.joins;
}

template<class Scheduler>
void stepping(Scheduler scheduler, std::thread::id only_worker, bool continuous)
{
    set_case(continuous ? "continuous" : "single-step");
    State state{continuous ? AdvanceMode::Continuous : AdvanceMode::SingleStep};
    state.only_worker = only_worker;
    auto root = take(state.source.make_root_branch());
    if (continuous) accept(state.source.request_pause());
    const bool value = run_scope_on(scheduler, state.changed, tree(std::move(root), state),
        [&](std::stop_token quit, auto&) {
            for (int round = 0; round != (continuous ? 1 : 4); ++round) {
                auto paused = wait_for_barrier(quit, state.changed, state.source);
                CHECK(paused && paused->paused_epoch);
                CHECK(state.visits == round * 4);
                CHECK(state.joins == 0);
                CHECK(paused->waiting_branch_count == (round == 3 ? 1u : 4u));
                accept(state.source.resume(*paused->paused_epoch,
                    continuous ? AdvanceMode::Continuous : AdvanceMode::SingleStep));
            }
        });
    CHECK(value && state.visits == 12 && state.finished == 4);
    CHECK(state.joins == 1 && state.result == 10);
    state.drained();
}

template<class Scheduler>
void stopping(Scheduler scheduler, bool prestopped, bool controller_error)
{
    set_case(prestopped ? "pre-stopped" : controller_error ? "controller-error" : "paused-stop");
    State state;
    bool threw = false;
    bool value = true;
    try {
        value = run_scope_on(scheduler, state.changed,
            tree(take(state.source.make_root_branch()), state),
            [&](std::stop_token quit, auto& stop) {
                auto paused = wait_for_barrier(quit, state.changed, state.source);
                if (prestopped) { CHECK(!paused); return; }
                CHECK(paused && paused->waiting_branch_count == 4);
                if (controller_error) throw ControllerError{};
                CHECK(stop.request_stop());
            }, prestopped);
    } catch (const ControllerError&) {
        threw = true;
    }
    CHECK(threw == controller_error);
    if (!controller_error) CHECK(!value);
    CHECK(state.visits == 0 && state.joins == 0 && state.finished == 0);
    if (prestopped) CHECK(state.started == 0);
    state.drained();
}

template<class Scheduler>
void business_error(Scheduler scheduler)
{
    set_case("business-error-cancels-siblings");
    State state;
    bool threw = false;
    try {
        (void)run_scope_on(scheduler, state.changed,
            tree(take(state.source.make_root_branch()), state, true),
            [&](std::stop_token quit, auto&) {
                auto paused = wait_for_barrier(quit, state.changed, state.source);
                CHECK(paused && paused->waiting_branch_count == 4);
                accept(state.source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));
            });
    } catch (const BusinessError&) {
        threw = true;
    }
    CHECK(threw && state.visits <= 3 && state.finished == 0 && state.joins == 0);
    state.drained();
}

stdexec::task<int> optional_first(PauseBranch branch, State& state,
                                  stdexec::inplace_stop_source& stop)
{
    const auto parent_token = co_await stdexec::read_env(stdexec::get_stop_token);
    auto forward = [&]() noexcept { (void)stop.request_stop(); };
    stdexec::inplace_stop_callback<decltype(forward)> bridge{parent_token, forward};
    auto value = co_await stdexec::stopped_as_optional(
        leaf(std::move(branch), state, 0)
        | stdexec::write_env(stdexec::prop{stdexec::get_stop_token, stop.get_token()}));
    // The temporary awaiter/frame has retired its branch before this publication.
    state.first_stopped.store(!value, std::memory_order_release);
    state.changed.signal();
    co_return value ? *value : 0;
}

stdexec::task<void> independent(PauseBranch first, PauseBranch second, State& state,
                                stdexec::inplace_stop_source& stop)
{
    auto [a, b] = co_await stdexec::when_all(
        optional_first(std::move(first), state, stop), leaf(std::move(second), state, 1));
    state.result = a + b;
}

template<class Scheduler>
void independent_stop(Scheduler scheduler)
{
    set_case("independent-stop");
    State state{AdvanceMode::Continuous};
    stdexec::inplace_stop_source stop;
    auto first = take(state.source.make_root_branch());
    auto second = take(state.source.make_branch());
    accept(state.source.request_pause());
    const bool value = run_scope_on(scheduler, state.changed,
        independent(std::move(first), std::move(second), state, stop),
        [&](std::stop_token quit, auto&) {
            auto paused = wait_for_barrier(quit, state.changed, state.source);
            CHECK(paused && paused->waiting_branch_count == 2);
            const auto epoch = paused->paused_epoch;
            CHECK(stop.request_stop());
            for (;;) {
                const auto observed = state.changed.sequence.load(std::memory_order_acquire);
                auto snapshot = state.source.snapshot();
                CHECK(!quit.stop_requested());
                if (state.first_stopped.load(std::memory_order_acquire)
                    && snapshot.live_branch_count == 1 && snapshot.waiting_branch_count == 1) {
                    CHECK(snapshot.paused_epoch == epoch && state.visits == 0);
                    accept(state.source.resume(*epoch, AdvanceMode::Continuous));
                    break;
                }
                state.changed.sequence.wait(observed, std::memory_order_acquire);
            }
        });
    CHECK(value && state.first_stopped && state.finished == 1);
    CHECK(state.visits == 3 && state.result == 2);
    state.drained();
}

template<class Scheduler>
void resume_stop_race(Scheduler scheduler)
{
    set_case("resume-stop-race");
    State state;
    (void)run_scope_on(scheduler, state.changed,
        tree(take(state.source.make_root_branch()), state),
        [&](std::stop_token quit, auto& stop) {
            auto paused = wait_for_barrier(quit, state.changed, state.source);
            CHECK(paused && paused->waiting_branch_count == 4);
            std::latch start{1};
            std::jthread resume([&] {
                start.wait();
                auto result = state.source.resume(*paused->paused_epoch, AdvanceMode::Continuous);
                CHECK(result || result.error() == PauseError::NotPaused
                             || result.error() == PauseError::StalePauseEpoch);
            });
            std::jthread cancel([&] { start.wait(); (void)stop.request_stop(); });
            start.count_down();
        });
    CHECK(state.visits <= 12 && state.joins <= 1);
    state.drained();
}

enum class Completion { None, Value, Stopped };
struct RawState {
    std::latch completed{1};
    Completion result = Completion::None;
    std::thread::id thread;
    int calls = 0;
};

template<class Scheduler>
struct RawReceiver {
    using receiver_concept = stdexec::receiver_tag;
    Scheduler scheduler;
    stdexec::inplace_stop_token token;
    RawState* state;
    auto get_env() const noexcept {
        return stdexec::env{
            stdexec::prop{stdexec::get_start_scheduler, scheduler},
            stdexec::prop{stdexec::get_stop_token, token}};
    }
    void complete(Completion result) noexcept {
        CHECK(++state->calls == 1);
        state->result = result;
        state->thread = std::this_thread::get_id();
        state->completed.count_down();
    }
    void set_value() noexcept { complete(Completion::Value); }
    void set_stopped() noexcept { complete(Completion::Stopped); }
    template<class Error> void set_error(Error&&) noexcept { CHECK(false); }
};

template<class Scheduler>
void raw_routes(Scheduler scheduler, int route, std::thread::id only_worker)
{
    set_case(route == 0 ? "raw-open" : route == 1 ? "raw-resume" : "raw-stop");
    State state{route == 0 ? AdvanceMode::Continuous : AdvanceMode::SingleStep};
    auto branch = take(state.source.make_root_branch());
    stdexec::inplace_stop_source stop;
    RawState result;
    auto operation = stdexec::connect(branch.suspend_point(),
        RawReceiver<Scheduler>{scheduler, stop.get_token(), &result});
    std::thread::id start_thread;
    CHECK(stdexec::sync_wait(stdexec::schedule(scheduler) | stdexec::then([&] {
        start_thread = std::this_thread::get_id();
        CHECK(start_thread != state.caller);
        stdexec::start(operation);
    })));
    if (route == 1) {
        const auto paused = state.source.snapshot();
        CHECK(paused.phase == GatePhase::Paused && paused.waiting_branch_count == 1);
        accept(state.source.resume(*paused.paused_epoch, AdvanceMode::Continuous));
    } else if (route == 2) {
        CHECK(stop.request_stop());
    }
    result.completed.wait();
    CHECK(result.calls == 1);
    CHECK(result.result == (route == 2 ? Completion::Stopped : Completion::Value));
    if (route == 1) {
        CHECK(result.thread != std::this_thread::get_id());
        if (only_worker != std::thread::id{}) CHECK(result.thread == only_worker);
    } else {
        CHECK(result.thread == (route == 0 ? start_thread : std::this_thread::get_id()));
    }
}

// Hold a selected external stop callback until the last when_all child has
// started unregistering it. The old dependency arrived at the barrier again
// from this callback, recursively destroying it and completing twice.
struct StopRace {
    stdexec::inplace_stop_source stop;
    std::latch selected{1}, removing{1}, release{1};
    std::atomic<int> removals{0};
};
struct HeldStopToken {
    StopRace* state;
    bool operator==(const HeldStopToken&) const noexcept = default;
    bool stop_possible() const noexcept { return true; }
    bool stop_requested() const noexcept { return state->stop.stop_requested(); }
    template<class Fn>
    struct callback_type {
        struct Invoke {
            StopRace* state;
            Fn fn;
            void operator()() noexcept {
                state->selected.count_down();
                state->release.wait();
                fn();
            }
        };
        StopRace* state;
        stdexec::inplace_stop_callback<Invoke> inner;
        callback_type(HeldStopToken token, Fn fn)
            : state(token.state), inner(state->stop.get_token(), Invoke{state, std::move(fn)}) {}
        ~callback_type() {
            CHECK(state->removals.fetch_add(1) == 0);
            state->removing.count_down();
        }
    };
};
struct RaceReceiver {
    using receiver_concept = stdexec::receiver_tag;
    StopRace* race;
    std::atomic<int>* completions;
    std::latch* completed;
    auto get_env() const noexcept { return stdexec::prop{stdexec::get_stop_token, HeldStopToken{race}}; }
    void set_value(int a, int b) noexcept {
        CHECK(a + b == 3);
        ++*completions;
        completed->count_down();
    }
    void set_stopped() noexcept { CHECK(false); }
    template<class Error> void set_error(Error&&) noexcept { CHECK(false); }
};
template<class Scheduler>
void when_all_stop_race(Scheduler scheduler)
{
    set_case("when-all-selected-stop-race");
    StopRace race;
    std::atomic<int> completions{0};
    std::latch ready{1}, finish{1}, completed{1};
    auto operation = stdexec::connect(stdexec::when_all(stdexec::just(1),
        stdexec::schedule(scheduler) | stdexec::then([&] {
            ready.count_down();
            finish.wait();
            return 2;
        })), RaceReceiver{&race, &completions, &completed});
    stdexec::start(operation);
    ready.wait();
    std::jthread cancel([&] { CHECK(race.stop.request_stop()); });
    race.selected.wait();
    finish.count_down();
    race.removing.wait();
    race.release.count_down();
    cancel.join();
    completed.wait();
    CHECK(completions == 1 && race.removals == 1);
}

// A composed ordinary sender must preserve the native pool's value domain,
// including Open inline completion and cancellation without the task adapter.
template<class Scheduler>
void pipeline_routes(Scheduler scheduler, std::thread::id only_worker)
{
    for (int route = 0; route != 3; ++route) {
        set_case(route == 0 ? "pipeline-open" : route == 1 ? "pipeline-resume" : "pipeline-stop");
        State state{route == 0 ? AdvanceMode::Continuous : AdvanceMode::SingleStep};
        {
            auto branch = take(state.source.make_root_branch());
            auto pipeline = branch.suspend_point() | stdexec::then([&] {
                CHECK(std::this_thread::get_id() != state.caller);
                if (only_worker != std::thread::id{}) CHECK(std::this_thread::get_id() == only_worker);
                ++state.visits;
            });
            const bool value = run_scope_on(scheduler, state.changed, std::move(pipeline),
                [&](std::stop_token quit, auto& stop) {
                    if (route == 0) return;
                    auto paused = wait_for_barrier(quit, state.changed, state.source);
                    CHECK(paused && paused->waiting_branch_count == 1);
                    if (route == 1) accept(state.source.resume(AdvanceMode::Continuous));
                    else CHECK(stop.request_stop());
                });
            CHECK(value == (route != 2) && state.visits == (route != 2 ? 1 : 0));
        }
        state.drained();
    }
}

struct Overlap {
    std::latch entered{2}, release{1};
    std::array<std::thread::id, 2> workers;
    std::atomic<int> active{0}, visits{0};
};

stdexec::task<int> concurrent_leaf(PauseBranch branch, Overlap& overlap, int index)
{
    co_await branch.suspend_point();
    overlap.workers[index] = std::this_thread::get_id();
    ++overlap.active;
    overlap.entered.count_down();
    overlap.release.wait(); // Test-only: run only with at least two workers.
    --overlap.active;
    ++overlap.visits;
    co_await branch.suspend_point();
    ++overlap.visits;
    co_return index + 1;
}

template<class Scheduler>
stdexec::task<void> concurrent_tree(PauseBranch parent, Scheduler first, Scheduler second, Overlap& overlap, State& state)
{
    auto children = take(parent.fork_and_block_self(2)); // Runtime-width fork.
    auto [a, b] = co_await stdexec::when_all(
        stdexec::starts_on(first, concurrent_leaf(std::move(children[0]), overlap, 0)),
        stdexec::starts_on(second, concurrent_leaf(std::move(children[1]), overlap, 1)));
    co_await parent.suspend_point();
    state.result = a + b;
}

void actual_parallelism(exec::static_thread_pool& pool)
{
    set_case("two-concurrent-workers");
    State state{AdvanceMode::Continuous};
    Overlap overlap;
    // Pin the two test jobs to distinct native workers. Nested unpinned schedule
    // can remain on a local queue and does not guarantee simultaneous execution.
    const bool value = run_scope_on(pool.get_scheduler(), state.changed,
        concurrent_tree(take(state.source.make_root_branch()), pool.get_scheduler_on_thread(0),
                        pool.get_scheduler_on_thread(1), overlap, state),
        [&](std::stop_token quit, auto&) {
            overlap.entered.wait();
            if (trace_cases) { std::puts("overlap: both workers entered"); std::fflush(stdout); }
            CHECK(overlap.active == 2 && overlap.visits == 0);
            CHECK(overlap.workers[0] != overlap.workers[1]);
            CHECK(overlap.workers[0] != state.caller && overlap.workers[1] != state.caller);
            accept(state.source.request_pause());
            CHECK(state.source.snapshot().phase == GatePhase::Collecting);
            overlap.release.count_down();
            auto paused = wait_for_barrier(quit, state.changed, state.source);
            CHECK(paused && paused->waiting_branch_count == 2 && overlap.visits == 2);
            if (trace_cases) { std::puts("overlap: both workers paused"); std::fflush(stdout); }
            const auto previous_epoch = paused->paused_epoch;
            accept(state.source.resume(*previous_epoch, AdvanceMode::SingleStep));
            paused = wait_for_barrier(quit, state.changed, state.source);
            CHECK(paused && paused->waiting_branch_count == 1 && overlap.visits == 4);
            CHECK(paused->paused_epoch != previous_epoch); // Parent's joined checkpoint.
            accept(state.source.resume(AdvanceMode::Continuous));
        });
    CHECK(value && state.result == 3 && overlap.active == 0 && overlap.visits == 4);
    state.drained();
}

template<class Scheduler>
void suite(Scheduler scheduler, int iterations, bool single_worker)
{
    static_assert(stdexec::scheduler<Scheduler>);
    auto worker = stdexec::sync_wait(stdexec::schedule(scheduler)
        | stdexec::then([] { return std::this_thread::get_id(); }));
    CHECK(worker && std::get<0>(*worker) != std::this_thread::get_id());
    const auto only_worker = single_worker ? std::get<0>(*worker) : std::thread::id{};
    for (int i = 0; i != iterations; ++i) {
        when_all_stop_race(scheduler);
        pipeline_routes(scheduler, only_worker);
        for (int route = 0; route != 3; ++route) raw_routes(scheduler, route, only_worker);
        stepping(scheduler, only_worker, false);
        stepping(scheduler, only_worker, true);
        stopping(scheduler, false, false);
        stopping(scheduler, true, false);
        stopping(scheduler, false, true);
        business_error(scheduler);
        independent_stop(scheduler);
        resume_stop_race(scheduler);
    }
}

int number(std::string_view text, int maximum)
{
    int value = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    CHECK(error == std::errc{} && end == text.data() + text.size() && value > 0 && value <= maximum);
    return value;
}
} // namespace

int main(int argc, char** argv)
{
    std::set_terminate([] {
        std::fprintf(stderr, "unexpected terminate: case=%s\n", current_case);
        std::_Exit(91);
    });
    std::string_view backend = "all";
    int threads = 2, iterations = 10;
    for (int i = 1; i < argc; i += 2) {
        CHECK(i + 1 < argc);
        const std::string_view option{argv[i]}, value{argv[i + 1]};
        if (option == "--backend") backend = value;
        else if (option == "--threads") threads = number(value, 64);
        else if (option == "--iterations") iterations = number(value, 1000000);
        else CHECK(false);
    }
    CHECK(backend == "all" || backend == "loop" || backend == "pool");
    if (backend != "pool") {
        RunLoopThread loop;
        suite(loop.get_scheduler(), iterations, true);
    }
    if (backend != "loop") {
        exec::static_thread_pool pool{static_cast<std::uint32_t>(threads)};
        suite(pool.get_scheduler(), iterations, threads == 1);
        if (threads > 1) for (int i = 0; i != iterations; ++i) actual_parallelism(pool);
    }
    std::printf("PASS: native schedulers backend=%.*s threads=%d iterations=%d\n",
                static_cast<int>(backend.size()), backend.data(), threads, iterations);
}
