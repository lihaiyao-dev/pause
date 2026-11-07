#define PAUSE_ENABLE_INDEPENDENT_BRANCH 1
/* 测试项目：pause_transition_extreme_test
 * 目的: 以真实 Pause 的显式合法调用顺序验证状态转换, 补充并发测试.
 * 测试项:
 * - 控制暂停/单步收集中逐个取消已等待分支, 保留尚未到达暂停点的分支责任.
 * - Open/Collecting/Paused 中最后子分支退役, 父分支随后重新进入暂停点.
 * - request_pause 在 fork 之前/之后提交的两种顺序.
 * - resume 与父分支重新进入的两种顺序, 分别组合 Continuous/SingleStep.
 * 关键预期: 共 11 项均核对 live/waiting/revision/epoch; 最后 child 退役即归还父责任,
 * 可直接再次 fork; Collecting 不得过早收敛; Open 同步放行而恢复路径只调度
 * 一次; 空屏障单步产生新 epoch, 旧 epoch 恢复失败且不改变 revision.
 * 入口: PAUSE_BUILD_TESTS=ON 时注册同名 CTest.
 *   ctest --test-dir <build> -C <config> -R "^pause_transition_extreme_test$" --output-on-failure
 *   pause_transition_extreme_test --iterations 10
 * 参数: iterations 为 1..1000000, 默认每项 10 次, 每次使用完整独立作用域.
 * 边界: 覆盖选定的状态锁提交顺序, 不是随机竞态或状态模型事件数; 不增加递归深度.
 */
#include "pause_sender_test_support.hpp"

#include <array>
#include <optional>

// Explicit serializations of legal state-lock orders complement the concurrent
// races. Every row is a complete real Pause scope, not a model event count.
namespace {
using namespace pause_sender_test;
using coop_pause::AdvanceMode;
using coop_pause::GatePhase;
using coop_pause::PauseBranch;
using coop_pause::PauseSource;
using Sender = decltype(std::declval<PauseBranch&>().suspend_point());
using Operation = stdexec::connect_result_t<Sender, Receiver>;

void partial_collecting_cancel(bool single_step)
{
    SchedulerState scheduler;
    stdexec::inplace_stop_source first_stop, second_stop;
    PauseSource source(single_step ? AdvanceMode::SingleStep : AdvanceMode::Continuous);
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
        CHECK(snapshot.origin == (single_step ? coop_pause::PauseOrigin::SingleStep
                                             : coop_pause::PauseOrigin::ControlRequest));
        const auto initial_revision = snapshot.revision;
        CHECK(first_stop.request_stop());
        CHECK(first.result == Completion::Stopped && second.result == Completion::None);
        snapshot = source.snapshot();
        CHECK(snapshot.phase == GatePhase::Collecting && !snapshot.paused_epoch);
        CHECK(snapshot.live_branch_count == 2 && snapshot.waiting_branch_count == 1);
        CHECK(snapshot.revision == initial_revision + 1);
        CHECK(second_stop.request_stop());
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
    PauseSource source(AdvanceMode::Continuous);
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
        CHECK(child_stop.request_stop());
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
        CHECK(retired.phase == (gate == GatePhase::Open ? GatePhase::Open : GatePhase::Collecting)
              && retired.waiting_branch_count == 0);
        CHECK(retired.live_branch_count == (gate == GatePhase::Collecting ? 2u : 1u));
        CHECK(retired.revision == before.revision + 1);
        CHECK(!retired.paused_epoch);
        // All records retired: the restored parent can immediately fork again.
        { auto again = root->fork_and_block_self<1>(); CHECK(again); }
        const auto reforked = source.snapshot();
        CHECK(reforked.revision == retired.revision + 2);
        CHECK(reforked.live_branch_count == retired.live_branch_count);
        parent_operation.reset(new Operation(stdexec::connect(
            root->suspend_point(), Receiver{&parent})));
        parent.start_active = true;
        stdexec::start(*parent_operation);
        parent.start_active = false;
        const auto reentered = source.snapshot();
        CHECK(reentered.live_branch_count == retired.live_branch_count);
        CHECK(reentered.revision == reforked.revision + (gate == GatePhase::Open ? 0u : 1u));
        CHECK(reentered.phase == gate);
        if (gate == GatePhase::Paused) CHECK(reentered.paused_epoch != before.paused_epoch);
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
    PauseSource source(AdvanceMode::Continuous);
    auto root = source.make_root_branch(); CHECK(root);
    if (pause_first) CHECK(source.request_pause());
    auto result = root->fork_and_block_self<3>(); CHECK(result);
    std::array<std::optional<PauseBranch>, 3> children;
    for (std::size_t index = 0; index != children.size(); ++index)
        children[index].emplace(std::move((*result)[index]));
    if (!pause_first) CHECK(source.request_pause());
    auto snapshot = source.snapshot();
    CHECK(snapshot.phase == GatePhase::Collecting && snapshot.revision == 3);
    CHECK(snapshot.origin == coop_pause::PauseOrigin::ControlRequest && !snapshot.step_round);
    CHECK(snapshot.live_branch_count == 3 && snapshot.waiting_branch_count == 0);
    for (std::size_t index = 0; index != children.size(); ++index) {
        children[index].reset();
        snapshot = source.snapshot();
        CHECK(snapshot.live_branch_count == (index + 1 == children.size() ? 1u : children.size() - index - 1));
        CHECK(snapshot.waiting_branch_count == 0 && snapshot.revision == 4 + index);
        CHECK(snapshot.phase == GatePhase::Collecting);
    }
    root = std::unexpected(coop_pause::PauseError::InvalidBranchState);
    snapshot = source.snapshot();
    CHECK(snapshot.phase == GatePhase::Paused && snapshot.paused_epoch->value == snapshot.revision);
    CHECK(source.resume(*snapshot.paused_epoch, AdvanceMode::Continuous));
    CHECK(source.snapshot().phase == GatePhase::Open);
    CHECK(source.snapshot().live_branch_count == 0);
}

// A branch withdrawn by accepted cancellation can legitimately join a paused
// epoch again. Parent handoff is separately tested above, and no longer creates
// an inactive branch that could enter an already-Paused epoch.
void reentry_resume_order(bool resume_first, AdvanceMode mode)
{
    SchedulerState scheduler;
    PauseSource source(AdvanceMode::Continuous);
    {
        auto root = source.make_root_branch(); CHECK(root);
        stdexec::inplace_stop_source cancelled;
        CHECK(cancelled.request_stop());
        ReceiverState parent{&scheduler, {}};
        std::unique_ptr<Operation> operation;
        run_in_context(scheduler, [&] {
            {
                ReceiverState stopped{&scheduler, cancelled.get_token()};
                stopped.finish_loop = false;
                auto cleanup = stdexec::connect(root->suspend_point(), Receiver{&stopped});
                stdexec::start(cleanup);
                CHECK(stopped.result == Completion::Stopped);
            }
            CHECK(source.snapshot().live_branch_count == 0);
            CHECK(source.request_pause());
            const auto old = source.snapshot();
            CHECK(old.phase == GatePhase::Paused && old.paused_epoch);
            if (resume_first) CHECK(source.resume(*old.paused_epoch, mode));
            const auto before = source.snapshot();
            operation.reset(new Operation(stdexec::connect(root->suspend_point(), Receiver{&parent})));
            parent.start_active = true;
            stdexec::start(*operation);
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
                    CHECK(entered.origin == coop_pause::PauseOrigin::SingleStep && entered.step_round == 1);
                    auto stale = source.resume(*old.paused_epoch, mode);
                    CHECK(!stale && stale.error() == coop_pause::PauseError::StalePauseEpoch);
                    CHECK(source.snapshot().revision == entered.revision);
                }
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
