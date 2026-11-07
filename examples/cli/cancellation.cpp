#include "example_support.hpp"
#include <exec/static_thread_pool.hpp>

#include <atomic>
#include <cstdio>

using namespace pause_examples;

stdexec::task<void> work(PauseBranch branch, std::atomic<int>& completed)
{
    co_await branch.suspend_point();
    ++completed; // Cancellation must leave this untouched.
}

int main()
{
    try {
        ChangeSignal changed;
        PauseSource source{AdvanceMode::SingleStep, phase_notifications(changed)};
        exec::static_thread_pool pool{2};
        std::atomic<int> completed{0};
        const bool value = run_scope_on(pool.get_scheduler(), changed,
            work(take(source.make_root_branch()), completed),
            [&](std::stop_token quit, auto& workflow_stop) {
                auto paused = wait_for_barrier(quit, changed, source);
                require(paused.has_value(), "expected a paused task");
                std::puts("control: cancel through the host stop source; no resume needed");
                (void)workflow_stop.request_stop();
            });
        require(!value && completed == 0, "cancelled task continued business work");
        require_drained(source);
        std::puts("task stopped; branches and operations released");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
