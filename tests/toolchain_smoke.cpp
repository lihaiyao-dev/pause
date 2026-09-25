/* 测试项目：C++23/stdexec 工具链运行冒烟。
 * 在独立 jthread 的 run_loop 上启动 task，co_await just(42)，核对 sync_wait 返回 42。
 * scope_guard 负责结束循环并 join；进程返回 0 为通过，空结果或错误值返回 1。
 * 构建开关：PAUSE_BUILD_TOOLCHAIN_SMOKE_TEST；CTest：pause_toolchain_smoke。
 * 仅检查协程、调度和链接环境，不覆盖 Pause 状态机，也不接受压力次数参数。
 */
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
