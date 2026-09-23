#include "pause_sender_test_support.hpp"

#include <array>
#include <optional>

// Explicit serializations of legal state-lock orders complement the concurrent
// races. Every row is a complete real Pause scope, not a model event count.
namespace {
using namespace pause_sender_test;
using pause::AdvanceMode;
using pause::GatePhase;
using pause::PauseBranch;
using pause::PauseSource;
using Sender = decltype(std::declval<PauseBranch&>().suspend_point());
using Operation = stdexec::connect_result_t<Sender, Receiver>;

void partial_collecting_cancel(bool single_step)
{
    SchedulerState scheduler;
    stdexec::inplace_stop_source first_stop, second_stop;
    PauseSource source(single_step ? AdvanceMode::SingleStep : AdvanceMode::Continuous,
                       []() noexcept {});
    auto root = source.make_root_branch(); CHECK(root);
    auto children = root->fork_and_block_self<3>(); CHECK(children);
    std::optional<PauseBranch> pending(std::move((*children)[2]));
    if (!single_step) CHECK(source.request_pause());
    ReceiverState first{&scheduler, first_stop.get_token()};
    ReceiverState second{&scheduler, second_stop.get_token()};
    first.finish_loop = second.finish_loop = false;
    auto first_operation = stdexec::connect((*children)[0].suspend_point(), Receiver{&first});
    auto second_operation = stdexec::connect((*children)[1].suspend_point(), Receiver{&second});
    run_in_context(scheduler, [&] {
        stdexec::start(first_operation);
        stdexec::start(second_operation);
        auto snapshot = source.snapshot();
        CHECK(snapshot.phase == GatePhase::Collecting && !snapshot.paused_epoch);
        CHECK(snapshot.live_branch_count == 3 && snapshot.waiting_branch_count == 2);
        CHECK(snapshot.origin == (single_step ? pause::PauseOrigin::SingleStep
                                             : pause::PauseOrigin::ControlRequest));
        const auto initial_revision = snapshot.revision;
        CHECK(!first_stop.request_stop());
        CHECK(first.result == Completion::Stopped && second.result == Completion::None);
        snapshot = source.snapshot();
        CHECK(snapshot.phase == GatePhase::Collecting && !snapshot.paused_epoch);
        CHECK(snapshot.live_branch_count == 2 && snapshot.waiting_branch_count == 1);
        CHECK(snapshot.revision == initial_revision + 1);
        CHECK(!second_stop.request_stop());
        snapshot = source.snapshot();
        CHECK(snapshot.phase == GatePhase::Collecting && !snapshot.paused_epoch);
        CHECK(snapshot.live_branch_count == 1 && snapshot.waiting_branch_count == 0);
        CHECK(snapshot.revision == initial_revision + 2);
        // Even cancellation of every existing waiter cannot discharge the
        // registered child that has never reached its first pause point.
        pending.reset();
        snapshot = source.snapshot();
        CHECK(snapshot.phase == GatePhase::Paused && snapshot.paused_epoch);
        CHECK(snapshot.live_branch_count == 0 && snapshot.waiting_branch_count == 0);
        CHECK(snapshot.revision == initial_revision + 3);
        CHECK(source.resume(*snapshot.paused_epoch, AdvanceMode::Continuous));
        CHECK(source.snapshot().phase == GatePhase::Open);
        scheduler.loop.finish();
    });
    CHECK(first.result == Completion::Stopped && second.result == Completion::Stopped);
    CHECK(scheduler.calls == 0 && scheduler.starts == 0);
}

void last_child_and_parent_reentry(GatePhase gate)
{
    SchedulerState scheduler;
    stdexec::inplace_stop_source child_stop;
    PauseSource source(AdvanceMode::Continuous, []() noexcept {});
    auto root = source.make_root_branch(); CHECK(root);
    std::optional<PauseBranch> pending;
    if (gate == GatePhase::Collecting) {
        auto independent = source.make_branch(); CHECK(independent);
        pending.emplace(std::move(*independent));
    }
    auto children = root->fork_and_block_self<1>(); CHECK(children);
    std::optional<PauseBranch> child(std::move((*children)[0]));
    if (gate != GatePhase::Open) CHECK(source.request_pause());
    ReceiverState child_receiver{&scheduler, child_stop.get_token()};
    child_receiver.finish_loop = false;
    std::unique_ptr<Operation> child_operation;
    if (gate == GatePhase::Paused) {
        CHECK(!child_stop.request_stop());
        child_operation.reset(new Operation(stdexec::connect(
            child->suspend_point(), Receiver{&child_receiver})));
    }
    ReceiverState parent{&scheduler, {}};
    std::unique_ptr<Operation> parent_operation;
    run_in_context(scheduler, [&] {
        if (child_operation) {
            stdexec::start(*child_operation);
            CHECK(child_receiver.result == Completion::Stopped);
            // End the completed operation's borrow before retiring the record.
            child_operation.reset();
        }
        const auto before = source.snapshot();
        CHECK(before.phase == gate && before.waiting_branch_count == 0);
        child.reset();
        const auto retired = source.snapshot();
        CHECK(retired.phase == gate && retired.waiting_branch_count == 0);
        CHECK(retired.live_branch_count == (gate == GatePhase::Collecting ? 1u : 0u));
        CHECK(retired.revision == before.revision + 1);
        CHECK(retired.paused_epoch == before.paused_epoch);
        // The direct-child count is zero, but retirement must not grant the
        // delegated parent permission to fork before its next pause point.
        auto rejected = root->fork_and_block_self<1>();
        CHECK(!rejected && rejected.error() == pause::PauseError::InvalidBranchState);
        CHECK(source.snapshot().revision == retired.revision);
        parent_operation.reset(new Operation(stdexec::connect(
            root->suspend_point(), Receiver{&parent})));
        parent.start_active = true;
        stdexec::start(*parent_operation);
        parent.start_active = false;
        const auto reentered = source.snapshot();
        CHECK(reentered.live_branch_count == retired.live_branch_count + 1);
        CHECK(reentered.revision == retired.revision + 1);
        CHECK(reentered.phase == gate && reentered.paused_epoch == retired.paused_epoch);
        if (gate == GatePhase::Open) {
            CHECK(parent.result == Completion::Value && parent.completed_in_start);
            CHECK(reentered.waiting_branch_count == 0);
        } else {
            CHECK(parent.result == Completion::None && reentered.waiting_branch_count == 1);
            if (gate == GatePhase::Collecting) {
                // Reentry arrives atomically and must not create a second
                // pending obligation beside the unrelated existing branch.
                pending.reset();
            }
            const auto paused = source.snapshot();
            CHECK(paused.phase == GatePhase::Paused && paused.paused_epoch);
            CHECK(paused.live_branch_count == 1 && paused.waiting_branch_count == 1);
            CHECK(source.resume(*paused.paused_epoch, AdvanceMode::Continuous));
            CHECK(parent.result == Completion::None);
        }
    });
    parent_operation.reset();
    CHECK(parent.result == Completion::Value);
    CHECK(scheduler.calls == (gate == GatePhase::Open ? 0 : 1));
    CHECK(scheduler.starts == (gate == GatePhase::Open ? 0 : 1));
    CHECK(source.snapshot().phase == GatePhase::Open);
    CHECK(source.snapshot().live_branch_count == 1);
}

void fork_pause_order(bool pause_first)
{
    PauseSource source(AdvanceMode::Continuous, []() noexcept {});
    auto root = source.make_root_branch(); CHECK(root);
    if (pause_first) CHECK(source.request_pause());
    auto result = root->fork_and_block_self<3>(); CHECK(result);
    std::array<std::optional<PauseBranch>, 3> children;
    for (std::size_t index = 0; index != children.size(); ++index)
        children[index].emplace(std::move((*result)[index]));
    if (!pause_first) CHECK(source.request_pause());
    auto snapshot = source.snapshot();
    CHECK(snapshot.phase == GatePhase::Collecting && snapshot.revision == 3);
    CHECK(snapshot.origin == pause::PauseOrigin::ControlRequest && !snapshot.step_round);
    CHECK(snapshot.live_branch_count == 3 && snapshot.waiting_branch_count == 0);
    for (std::size_t index = 0; index != children.size(); ++index) {
        children[index].reset();
        snapshot = source.snapshot();
        CHECK(snapshot.live_branch_count == children.size() - index - 1);
        CHECK(snapshot.waiting_branch_count == 0 && snapshot.revision == 4 + index);
        CHECK(snapshot.phase == (index + 1 == children.size()
                                    ? GatePhase::Paused : GatePhase::Collecting));
    }
    CHECK(snapshot.paused_epoch && snapshot.paused_epoch->value == snapshot.revision);
    CHECK(source.resume(*snapshot.paused_epoch, AdvanceMode::Continuous));
    CHECK(source.snapshot().phase == GatePhase::Open);
    CHECK(source.snapshot().live_branch_count == 0);
}

void reentry_resume_order(bool resume_first, AdvanceMode mode)
{
    SchedulerState scheduler;
    PauseSource source(AdvanceMode::Continuous, []() noexcept {});
    {
        auto root = source.make_root_branch(); CHECK(root);
        {
            auto child = root->fork_and_block_self<1>(); CHECK(child);
        }
        CHECK(source.snapshot().live_branch_count == 0);
        CHECK(source.request_pause());
        const auto old = source.snapshot();
        CHECK(old.phase == GatePhase::Paused && old.paused_epoch);
        ReceiverState parent{&scheduler, {}};
        auto operation = stdexec::connect(root->suspend_point(), Receiver{&parent});
        run_in_context(scheduler, [&] {
            if (resume_first) CHECK(source.resume(*old.paused_epoch, mode));
            const auto before = source.snapshot();
            parent.start_active = true;
            stdexec::start(operation);
            parent.start_active = false;
            const auto entered = source.snapshot();
            CHECK(entered.live_branch_count == 1 && entered.revision == before.revision + 1);
            CHECK(entered.paused_epoch == before.paused_epoch);
            if (resume_first && mode == AdvanceMode::Continuous) {
                CHECK(entered.phase == GatePhase::Open && entered.waiting_branch_count == 0);
                CHECK(parent.result == Completion::Value && parent.completed_in_start);
            } else {
                CHECK(entered.phase == GatePhase::Paused && entered.waiting_branch_count == 1);
                CHECK(parent.result == Completion::None);
                if (resume_first) {
                    CHECK(entered.paused_epoch != old.paused_epoch);
                    CHECK(entered.origin == pause::PauseOrigin::SingleStep && entered.step_round == 1);
                    auto stale = source.resume(*old.paused_epoch, mode);
                    CHECK(!stale && stale.error() == pause::PauseError::StalePauseEpoch);
                    CHECK(source.snapshot().revision == entered.revision);
                } else CHECK(entered.paused_epoch == old.paused_epoch);
                CHECK(source.resume(*entered.paused_epoch,
                                    resume_first ? AdvanceMode::Continuous : mode));
                CHECK(parent.result == Completion::None);
            }
        });
        CHECK(parent.result == Completion::Value);
        const bool inline_pass = resume_first && mode == AdvanceMode::Continuous;
        CHECK(scheduler.calls == (inline_pass ? 0 : 1));
        CHECK(scheduler.starts == (inline_pass ? 0 : 1));
        const auto completed = source.snapshot();
        CHECK(completed.live_branch_count == 1 && completed.waiting_branch_count == 0);
        CHECK(completed.phase == (!resume_first && mode == AdvanceMode::SingleStep
                                     ? GatePhase::Collecting : GatePhase::Open));
    }
    const auto drained = source.snapshot();
    CHECK(drained.live_branch_count == 0 && drained.waiting_branch_count == 0);
    CHECK(drained.phase == (!resume_first && mode == AdvanceMode::SingleStep
                               ? GatePhase::Paused : GatePhase::Open));
}

template<class Function>
void run_case(const char* name, int count, Function function)
{
    for (int iteration = 0; iteration != count; ++iteration) function();
    std::printf("PASS transition=%s complete_scopes=%d\n", name, count);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    const int count = argc == 1 ? 10 : pause_sender_test::iterations(argc, argv);
    run_case("collecting-control-partial-cancel", count, [] { partial_collecting_cancel(false); });
    run_case("collecting-step-partial-cancel", count, [] { partial_collecting_cancel(true); });
    run_case("last-child-reentry-open", count, [] { last_child_and_parent_reentry(GatePhase::Open); });
    run_case("last-child-reentry-collecting", count, [] { last_child_and_parent_reentry(GatePhase::Collecting); });
    run_case("last-child-reentry-paused", count, [] { last_child_and_parent_reentry(GatePhase::Paused); });
    run_case("pause-before-fork", count, [] { fork_pause_order(true); });
    run_case("fork-before-pause", count, [] { fork_pause_order(false); });
    run_case("resume-before-reentry-continuous", count, [] { reentry_resume_order(true, AdvanceMode::Continuous); });
    run_case("resume-before-reentry-step", count, [] { reentry_resume_order(true, AdvanceMode::SingleStep); });
    run_case("reentry-before-resume-continuous", count, [] { reentry_resume_order(false, AdvanceMode::Continuous); });
    run_case("reentry-before-resume-step", count, [] { reentry_resume_order(false, AdvanceMode::SingleStep); });
    std::printf("real Pause directed transitions: %d complete scopes, 11 state families, %d repetitions each\n",
                count * 11, count);
}
