#include <exec/scope.hpp>
#include <stdexec/execution.hpp>

#include <thread>
#include <tuple>

stdexec::task<int> smoke_task()
{
    co_return co_await stdexec::just(42);
}

int main()
{
    stdexec::run_loop workers;
    std::jthread worker([&]() noexcept { workers.run(); });
    exec::scope_guard finish_worker{[&]() noexcept {
        workers.finish();
        worker.join();
    }};
    auto result = stdexec::sync_wait(
        stdexec::starts_on(workers.get_scheduler(), smoke_task()));
    return result && std::get<0>(*result) == 42 ? 0 : 1;
}
