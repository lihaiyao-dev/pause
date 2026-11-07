#include <pause.hpp>

#ifdef PAUSE_TEST_THREAD_POOL
#include <exec/static_thread_pool.hpp>
#include <atomic>
#include <thread>

stdexec::task<void> probe(coop_pause::PauseBranch branch, int& visits)
{
    co_await branch.suspend_point();
    ++visits;
}

template<stdexec::scheduler Scheduler>
bool native_probe(Scheduler scheduler)
{
    std::atomic<unsigned> changed{0};
    auto notify = [&]() noexcept { ++changed; changed.notify_all(); };
    coop_pause::PauseSource source(coop_pause::AdvanceMode::SingleStep, coop_pause::PausePhaseNotifications{notify, notify, notify});
    auto root = source.make_root_branch();
    if (!root) return false;
    std::jthread controller([&](std::stop_token quit) {
        for (;;) {
            const auto observed = changed.load(std::memory_order_acquire);
            if (quit.stop_requested()) return;
            const auto state = source.snapshot();
            if (state.phase == coop_pause::GatePhase::Paused && state.waiting_branch_count == 1) {
                if (!source.resume(*state.paused_epoch, coop_pause::AdvanceMode::Continuous))
                    std::terminate();
                return;
            }
            changed.wait(observed, std::memory_order_acquire);
        }
    });
    int visits = 0;
    auto value = stdexec::sync_wait(stdexec::starts_on(
        scheduler, probe(std::move(*root), visits)));
    controller.request_stop();
    notify();
    controller.join();
    return value && visits == 1 && source.snapshot().live_branch_count == 0;
}
#endif

int main()
{
#ifdef PAUSE_TEST_THREAD_POOL
    exec::static_thread_pool pool{1};
    if (!native_probe(pool.get_scheduler())) return 3;
#endif
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous);
    auto root = source.make_root_branch();
    if (!root) return 1;
    const auto state = source.snapshot();
    return state.root_created && state.phase == coop_pause::GatePhase::Open ? 0 : 2;
}
