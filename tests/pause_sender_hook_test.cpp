namespace pause_test_hooks {
void after_registration() noexcept;
void after_selection() noexcept;
void inside_remove_wait() noexcept;
}
#include "pause_sender_test_support.hpp"

namespace {
using namespace pause_sender_test;
struct HookState {
    bool hold_registration = false, hold_selection = false;
    std::latch registered{1}, release_registration{1};
    std::latch selected{1}, release_selection{1}, removing{1};
    std::atomic<bool> removing_reported{false};
    std::atomic<int> registrations{0}, selections{0};
};
// Installed before any thread starts, cleared only after all users join.
HookState* hooks = nullptr;
using Sender = decltype(std::declval<pause::PauseBranch&>().suspend_point());
using Operation = stdexec::connect_result_t<Sender, Receiver>;

void registered_before_emplace_returns()
{
    HookState state; state.hold_registration = true; hooks = &state;
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    pause::PauseSource source(pause::AdvanceMode::SingleStep, []() noexcept {});
    auto root = source.make_root_branch(); CHECK(root);
    ReceiverState receiver{&scheduler, stop.get_token()};
    auto operation = stdexec::connect(root->suspend_point(), Receiver{&receiver});
    std::jthread starter([&] { run_in_context(scheduler, [&] { stdexec::start(operation); }); });
    state.registered.wait();
    CHECK(!stop.request_stop());
    CHECK(state.selections == 1);
    CHECK(receiver.result == Completion::None);
    CHECK(source.snapshot().live_branch_count == 1); // Only CancelRequested so far.
    CHECK(source.snapshot().waiting_branch_count == 0);
    state.release_registration.count_down();
    starter.join();
    CHECK(receiver.result == Completion::Stopped);
    CHECK(source.snapshot().live_branch_count == 0);
    CHECK(scheduler.calls == 0 && scheduler.starts == 0);
    hooks = nullptr;
}

void selected_during_construction(bool open)
{
    HookState state; state.hold_registration = state.hold_selection = true; hooks = &state;
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    std::latch waiting{1};
    std::atomic<bool> armed{false};
    pause::PauseSource source(open ? pause::AdvanceMode::Continuous : pause::AdvanceMode::SingleStep,
        [&]() noexcept { if (armed.exchange(false)) waiting.count_down(); });
    auto root = source.make_root_branch(); CHECK(root);
    ReceiverState receiver{&scheduler, stop.get_token()};
    auto operation = stdexec::connect(root->suspend_point(), Receiver{&receiver});
    armed = true;
    std::jthread starter([&] { run_in_context(scheduler, [&] { stdexec::start(operation); }); });
    state.registered.wait();
    std::jthread requester([&] { CHECK(!stop.request_stop()); });
    state.selected.wait();
    state.release_registration.count_down();
    if (open) state.removing.wait();
    else waiting.wait();
    CHECK(receiver.result == Completion::None);
    CHECK(source.snapshot().waiting_branch_count == (open ? 0 : 1));
    if (open) {
        // start already claimed InlinePass, but the selected callback keeps
        // reset blocked. A later pause cannot change that completed decision.
        CHECK(source.request_pause());
        CHECK(source.snapshot().phase == pause::GatePhase::Collecting);
    }
    state.release_selection.count_down();
    requester.join(); starter.join();
    CHECK(receiver.result == (open ? Completion::Value : Completion::Stopped));
    CHECK(scheduler.calls == 0 && scheduler.starts == 0);
    CHECK(source.snapshot().live_branch_count == (open ? 1 : 0));
    hooks = nullptr;
}

void resume_unregisters_selected_callback()
{
    HookState state; state.hold_selection = true; hooks = &state;
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    std::latch waiting{1};
    std::atomic<bool> armed{false};
    pause::PauseSource source(pause::AdvanceMode::SingleStep,
        [&]() noexcept { if (armed.exchange(false)) waiting.count_down(); });
    auto root = source.make_root_branch(); CHECK(root);
    ReceiverState receiver{&scheduler, stop.get_token()};
    auto operation = stdexec::connect(root->suspend_point(), Receiver{&receiver});
    armed = true;
    std::jthread starter([&] { run_in_context(scheduler, [&] { stdexec::start(operation); }); });
    waiting.wait();
    const auto epoch = *source.snapshot().paused_epoch;
    std::jthread requester([&] { CHECK(!stop.request_stop()); });
    state.selected.wait();
    std::jthread resumer([&] { CHECK(source.resume(epoch, pause::AdvanceMode::SingleStep)); });
    state.removing.wait();
    CHECK(receiver.result == Completion::None);
    CHECK(source.snapshot().waiting_branch_count == 0);
    CHECK(scheduler.calls == 0); // Reset waits outside the Pause mutex.
    state.release_selection.count_down();
    requester.join(); resumer.join(); starter.join();
    CHECK(receiver.result == Completion::Stopped); // Resume won raw; queue observes stop.
    CHECK(scheduler.calls == 1 && scheduler.starts == 1);
    CHECK(source.snapshot().live_branch_count == 0);
    CHECK(source.snapshot().phase == pause::GatePhase::Paused);
    hooks = nullptr;
}

void callback_completion_destroyed_elsewhere()
{
    HookState state; hooks = &state;
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    std::latch waiting{1};
    std::atomic<bool> armed{false}, destroyed{false};
    Gate tail;
    pause::PauseSource source(pause::AdvanceMode::SingleStep,
        [&]() noexcept { if (armed.exchange(false)) waiting.count_down(); });
    auto root = source.make_root_branch(); CHECK(root);
    ReceiverState receiver{&scheduler, stop.get_token()};
    receiver.completion_tail = &tail;
    auto* operation = new Operation(stdexec::connect(root->suspend_point(), Receiver{&receiver}));
    armed = true;
    std::jthread starter([&] { run_in_context(scheduler, [&] { stdexec::start(*operation); }); });
    waiting.wait();
    std::jthread requester([&] { CHECK(!stop.request_stop()); });
    tail.entered.wait();
    CHECK(receiver.result == Completion::Stopped);
    CHECK(source.snapshot().live_branch_count == 0);
    // The second request is not a join of the first callback/request stack.
    CHECK(stop.request_stop());
    std::jthread destroyer([&] { delete operation; destroyed = true; });
    state.removing.wait();
    CHECK(!destroyed);
    tail.release.count_down();
    requester.join(); destroyer.join(); starter.join();
    CHECK(destroyed && state.selections == 1);
    hooks = nullptr;
}

void unregister_before_request()
{
    HookState state; hooks = &state;
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    pause::PauseSource source(pause::AdvanceMode::Continuous, []() noexcept {});
    auto root = source.make_root_branch(); CHECK(root);
    ReceiverState receiver{&scheduler, stop.get_token()};
    auto operation = stdexec::connect(root->suspend_point(), Receiver{&receiver});
    run_in_context(scheduler, [&] { stdexec::start(operation); });
    CHECK(state.registrations == 1 && state.selections == 0);
    CHECK(!stop.request_stop());
    CHECK(state.selections == 0 && receiver.result == Completion::Value);
    hooks = nullptr;
}

void resume_unregisters_entire_batch()
{
    HookState state; hooks = &state;
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    pause::PauseSource source(pause::AdvanceMode::Continuous, []() noexcept {});
    auto first = source.make_root_branch(); CHECK(first);
    auto second = source.make_branch(); CHECK(second);
    CHECK(source.request_pause());
    std::atomic<int> completions{0};
    struct BatchReceiverState : ReceiverState { std::atomic<int>* completions; };
    BatchReceiverState a{{&scheduler, stop.get_token()}, &completions};
    BatchReceiverState b{{&scheduler, stop.get_token()}, &completions};
    a.finish_loop = b.finish_loop = false;
    a.on_completion = b.on_completion = [](ReceiverState* receiver) noexcept {
        auto* current = static_cast<BatchReceiverState*>(receiver);
        if (++*current->completions == 2) current->scheduler->loop.finish();
    };
    auto one = stdexec::connect(first->suspend_point(), Receiver{&a});
    auto two = stdexec::connect(second->suspend_point(), Receiver{&b});
    std::latch waiting{1};
    struct Context { stdexec::inplace_stop_source* stop; HookState* hooks; } context{&stop, &state};
    std::jthread starter([&] {
        run_in_context(scheduler, [&] {
            stdexec::start(one); stdexec::start(two);
            waiting.count_down();
        });
    });
    waiting.wait();
    CHECK(source.snapshot().waiting_branch_count == 2);
    scheduler.schedule_context = &context;
    scheduler.before_schedule = [](SchedulerState* current) noexcept {
        auto* local = static_cast<Context*>(current->schedule_context);
        // Executed synchronously while delivering the first Resume batch node.
        // Both callbacks must already be unregistered, including the second node.
        if (current->calls == 1) {
            CHECK(!local->stop->request_stop());
            CHECK(local->hooks->selections == 0);
        }
    };
    CHECK(source.resume(*source.snapshot().paused_epoch, pause::AdvanceMode::SingleStep));
    starter.join();
    CHECK(a.result == Completion::Stopped && b.result == Completion::Stopped);
    CHECK(scheduler.calls == 2 && scheduler.starts == 2);
    CHECK(state.registrations == 2 && state.selections == 0);
    CHECK(source.snapshot().live_branch_count == 0);
    CHECK(source.snapshot().phase == pause::GatePhase::Paused);
    hooks = nullptr;
}
} // namespace

namespace pause_test_hooks {
void after_registration() noexcept {
    if (!hooks) return;
    ++hooks->registrations;
    if (hooks->hold_registration) {
        hooks->registered.count_down(); hooks->release_registration.wait();
    }
}
void after_selection() noexcept {
    if (!hooks) return;
    ++hooks->selections;
    if (hooks->hold_selection) {
        hooks->selected.count_down(); hooks->release_selection.wait();
    }
}
void inside_remove_wait() noexcept {
    if (hooks && !hooks->removing_reported.exchange(true)) hooks->removing.count_down();
}
} // namespace pause_test_hooks

int main(int argc, char** argv) {
    const int count = iterations(argc, argv);
    for (int i = 0; i != count; ++i) {
        registered_before_emplace_returns();
        selected_during_construction(false);
        selected_during_construction(true);
        resume_unregisters_selected_callback();
        callback_completion_destroyed_elsewhere();
        unregister_before_request();
        resume_unregisters_entire_batch();
    }
    std::printf("PASS: 7 instrumented real Pause callback/lifetime windows x %d iterations\n", count);
}
