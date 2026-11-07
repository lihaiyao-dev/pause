#include "example_support.hpp"
#include <exec/static_thread_pool.hpp>

#include <atomic>
#include <charconv>
#include <cstdio>
#include <string_view>

using namespace pause_examples;

stdexec::task<int> shard(PauseBranch branch, int id, std::atomic<int>& completed)
{
    for (int stage = 0; stage != 2; ++stage) {
        co_await branch.suspend_point();
        ++completed;
        std::printf("shard %d: stage %d\n", id, stage + 1);
    }
    co_return id;
}

template<stdexec::scheduler Scheduler>
stdexec::task<void> parallel_work(PauseBranch parent, Scheduler scheduler,
                                 std::atomic<int>& completed, int& result)
{
    co_await parent.suspend_point();
    auto children = take(parent.fork_and_block_self<2>());
    // Each path owns a branch. Submit each task to the pool explicitly:
    // when_all alone expresses a join, not a guarantee of another worker.
    auto [left, right] = co_await stdexec::when_all(
        stdexec::starts_on(scheduler, shard(std::move(children[0]), 1, completed)),
        stdexec::starts_on(scheduler, shard(std::move(children[1]), 2, completed)));
    // The last child has returned the responsibility to parent.
    co_await parent.suspend_point();
    result = left + right;
}

int main(int argc, char** argv)
{
    try {
        require(argc <= 2, "usage: pause_thread_pool [1..64]");
        unsigned threads = 4;
        if (argc == 2) {
            const std::string_view text{argv[1]};
            const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), threads);
            require(error == std::errc{} && end == text.data() + text.size()
                        && threads >= 1 && threads <= 64, "expected 1..64 workers");
        }
        ChangeSignal changed;
        PauseSource source{AdvanceMode::SingleStep, phase_notifications(changed)};
        exec::static_thread_pool pool{threads};
        std::atomic<int> completed{0};
        int result = 0;
        const bool value = run_scope_on(pool.get_scheduler(), changed,
            parallel_work(take(source.make_root_branch()), pool.get_scheduler(), completed, result),
            [&](std::stop_token quit, auto&) {
                // Entry, two child stages, then the parent's join checkpoint.
                for (int round = 0; round != 4; ++round) {
                    auto paused = wait_for_barrier(quit, changed, source);
                    require(paused.has_value(), "workflow ended before its checkpoint");
                    std::printf("control: round=%d waiters=%zu completed=%d\n",
                                round, paused->waiting_branch_count, completed.load());
                    accept(source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));
                }
            });
        require(value && completed == 4 && result == 3, "incomplete parallel workflow");
        require_drained(source);
        std::printf("joined: result=%d\n", result);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
