// Current default API and phase-only notifications. No independent-branch macro.
#include "pause_sender_test_support.hpp"

#include <array>
#include <optional>

namespace {
using namespace coop_pause;
using namespace pause_sender_test;

template<class T>
concept HasIndependentBranch = requires(T& source) { source.make_branch(); };
static_assert(!HasIndependentBranch<PauseSource>);

struct Notifications {
    std::array<int, 3> count{};
    PausePhaseNotifications callbacks()
    {
        return {[this]() noexcept { ++count[0]; },
                [this]() noexcept { ++count[1]; },
                [this]() noexcept { ++count[2]; }};
    }
};

struct QueryReceiver {
    using receiver_concept = stdexec::receiver_tag;
    ReceiverState* result;
    int* queries;
    struct Env {
        ReceiverState* result;
        int* queries;
        auto query(stdexec::get_start_scheduler_t) const noexcept
        {
            ++*queries;
            return CountingScheduler{result->scheduler};
        }
        auto query(stdexec::get_stop_token_t) const noexcept { return result->token; }
    };
    auto get_env() const noexcept { return Env{result, queries}; }
    void set_value() noexcept { Receiver{result}.set_value(); }
    void set_stopped() noexcept { Receiver{result}.set_stopped(); }
    template<class Error> void set_error(Error&&) noexcept { CHECK(false); }
};

void scheduler_queries(bool closed)
{
    SchedulerState scheduler;
    PauseSource source{closed ? AdvanceMode::SingleStep : AdvanceMode::Continuous};
    auto root = source.make_root_branch(); CHECK(root);
    ReceiverState result{&scheduler, {}};
    int queries = 0;
    auto operation = stdexec::connect(root->suspend_point(), QueryReceiver{&result, &queries});
    CHECK(queries == 0);
    run_in_context(scheduler, [&] {
        stdexec::start(operation);
        CHECK(queries == 0); // Neither connect nor the Open/Waiting decision reads it.
        if (closed) {
            CHECK(source.snapshot().phase == GatePhase::Paused);
            CHECK(source.resume(AdvanceMode::Continuous));
            CHECK(queries > 0); // Resume reads it; stdexec adaptors may also query it.
        }
    });
    CHECK(result.result == Completion::Value);
    CHECK(scheduler.calls == (closed ? 1 : 0) && scheduler.starts == (closed ? 1 : 0));
}

void empty_barriers()
{
    PauseSource silent{AdvanceMode::Continuous};
    auto no_root = silent.resume(AdvanceMode::Continuous);
    CHECK(!no_root && no_root.error() == PauseError::RootNotCreated);
    { auto root = silent.make_root_branch(); CHECK(root); }
    CHECK(silent.request_pause());
    CHECK(silent.resume(AdvanceMode::Continuous));

    Notifications events;
    PauseSource source{AdvanceMode::Continuous, events.callbacks()};
    { auto root = source.make_root_branch(); CHECK(root); }
    CHECK((events.count == std::array{0, 0, 0}));
    auto open = source.resume(AdvanceMode::Continuous);
    CHECK(!open && open.error() == PauseError::NotPaused);
    CHECK(source.request_pause()); // Open -> Paused; no intermediate notification.
    CHECK((events.count == std::array{0, 0, 1}));
    const auto old = source.snapshot();
    CHECK(source.resume(AdvanceMode::SingleStep));
    const auto next = source.snapshot();
    CHECK(next.phase == GatePhase::Paused && next.paused_epoch != old.paused_epoch);
    CHECK(next.revision == old.revision + 1 && next.step_round == 1);
    CHECK((events.count == std::array{0, 0, 1}));
    auto stale = source.resume(*old.paused_epoch, AdvanceMode::Continuous);
    CHECK(!stale && stale.error() == PauseError::StalePauseEpoch);
    CHECK(source.resume(AdvanceMode::Continuous));
    CHECK((events.count == std::array{1, 0, 1}));
}

void parent_return(GatePhase gate)
{
    Notifications events;
    PauseSource source{AdvanceMode::Continuous, events.callbacks()};
    std::optional<PauseBranch> root;
    auto created = source.make_root_branch(); CHECK(created);
    root.emplace(std::move(*created));
    auto forked = root->fork_and_block_self<1>(); CHECK(forked);
    std::optional<PauseBranch> child{std::move((*forked)[0])};
    if (gate != GatePhase::Open) CHECK(source.request_pause());
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    if (gate == GatePhase::Paused) {
        CHECK(stop.request_stop());
        ReceiverState receiver{&scheduler, stop.get_token()};
        auto operation = stdexec::connect(child->suspend_point(), Receiver{&receiver});
        run_in_context(scheduler, [&] { stdexec::start(operation); });
        CHECK(receiver.result == Completion::Stopped);
    }
    const auto before = source.snapshot();
    const auto previous_events = events.count;
    child.reset();
    const auto after = source.snapshot();
    CHECK(after.live_branch_count == 1 && after.waiting_branch_count == 0);
    CHECK(after.revision == before.revision + 1);
    CHECK(after.origin == before.origin && after.step_round == before.step_round);
    CHECK(after.phase == (gate == GatePhase::Open ? GatePhase::Open : GatePhase::Collecting));
    CHECK(!after.paused_epoch);
    CHECK(events.count[1] == previous_events[1] + (gate == GatePhase::Paused ? 1 : 0));
    // Responsibility is already back: fork again without first awaiting a point.
    { auto again = root->fork_and_block_self(2); CHECK(again && again->size() == 2); }
    CHECK(source.snapshot().live_branch_count == 1);
    CHECK(source.snapshot().phase == after.phase);
    root.reset();
    CHECK(source.snapshot().live_branch_count == 0);
    CHECK(source.snapshot().phase == (gate == GatePhase::Open ? GatePhase::Open : GatePhase::Paused));
}
} // namespace

int main(int argc, char** argv)
{
    const int count = iterations(argc, argv);
    for (int i = 0; i != count; ++i) {
        empty_barriers();
        scheduler_queries(false);
        scheduler_queries(true);
        for (auto gate : {GatePhase::Open, GatePhase::Collecting, GatePhase::Paused}) parent_return(gate);
    }
    std::puts("PASS default API, phase notifications, epochs, parent responsibility return");
}
