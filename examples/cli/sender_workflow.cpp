#include "example_support.hpp"
#include <exec/static_thread_pool.hpp>

#include <cstdio>

using namespace pause_examples;

int main()
{
    try {
        ChangeSignal changed;
        PauseSource source{AdvanceMode::SingleStep, phase_notifications(changed)};
        exec::static_thread_pool pool{2};
        int result = 0;
        {
            // The sender borrows branch; keep it alive until the complete
            // operation and every control call have been joined.
            auto branch = take(source.make_root_branch());
            auto pipeline = branch.suspend_point()
                | stdexec::then([&] { result = 42; std::puts("sender: value=42"); });
            const bool value = run_scope_on(pool.get_scheduler(), changed, std::move(pipeline),
                [&](std::stop_token quit, auto&) {
                    auto paused = wait_for_barrier(quit, changed, source);
                    require(paused.has_value(), "expected the sender to wait");
                    std::puts("control: resume the ordinary sender pipeline");
                    accept(source.resume(*paused->paused_epoch, AdvanceMode::Continuous));
                });
            require(value && result == 42, "sender did not finish");
        }
        require_drained(source);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
