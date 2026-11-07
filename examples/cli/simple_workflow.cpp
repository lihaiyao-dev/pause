#include "example_support.hpp"

#include <atomic>
#include <cstdio>

using namespace pause_examples;

// Each pass is one business unit. Pause only at an explicit boundary.
stdexec::task<void> count_batches(PauseBranch branch, std::atomic<int>& completed)
{
    for (int batch = 1; batch <= 3; ++batch) {
        co_await branch.suspend_point();
        ++completed;
        std::printf("work: batch %d\n", batch);
    }
}

int main()
{
    try {
        ChangeSignal changed;
        PauseSource source{AdvanceMode::Continuous, phase_notifications(changed)};
        std::atomic<int> completed{0};
        auto root = take(source.make_root_branch());

        // Close the gate. The task still has to reach its entry point.
        accept(source.request_pause());
        const bool value = run_scope(changed, count_batches(std::move(root), completed),
            [&](std::stop_token quit, auto&) {
                auto paused = wait_for_barrier(quit, changed, source);
                require(paused && completed == 0, "expected entry pause");
                std::puts("control: paused before the first batch; step once");
                accept(source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));

                paused = wait_for_barrier(quit, changed, source);
                require(paused && completed == 1, "single step crossed a boundary");
                std::puts("control: one batch completed; continue");
                accept(source.resume(*paused->paused_epoch, AdvanceMode::Continuous));
            });
        require(value && completed == 3, "workflow did not finish");
        require_drained(source);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
