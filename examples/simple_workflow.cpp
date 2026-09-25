#include "example_support.hpp"

#include <exec/timed_thread_scheduler.hpp>

#include <chrono>
#include <cstdio>
#include <utility>

namespace {
using namespace pause_examples;
using namespace std::chrono_literals;

constexpr int total_steps = 6;

struct Progress {
    int slow_steps = 0;
    bool fast_done = false;
    bool joined = false;
    int result = 0;
};

stdexec::task<void> work_task(PauseBranch branch, exec::timed_thread_scheduler timer,
                            Progress& progress)
{
    co_await branch.suspend_point();
    std::puts("[work] awaiting when_all(fast, slow)");
    auto [fast, slow] = take(branch.fork_and_block_self<2>());
    // 子流程直接写在业务中。协程 lambda 无捕获，branch 按值进入各自的 frame。
    auto [cached, published] = co_await stdexec::when_all(
        [](PauseBranch child, exec::timed_thread_scheduler timer,
           Progress& progress) -> stdexec::task<int> {
            co_await child.suspend_point();
            std::puts("[fast] begin (sleep 300ms)");
            co_await exec::schedule_after(timer, 300ms);
            progress.fast_done = true;
            std::puts("[fast] done: cached=10; when_all still needs slow");
            co_return 10;
        }(std::move(fast), timer, progress),
        [](PauseBranch child, exec::timed_thread_scheduler timer,
           Progress& progress) -> stdexec::task<int> {
            // 1. then：异步加载后产生值；共享进度只在 await 返回后更新。
            std::puts("[slow] checkpoint 1: load");
            co_await child.suspend_point();
            std::puts("[slow] 1. load: begin (500ms)");
            const int loaded = co_await (exec::schedule_after(timer, 500ms)
                | stdexec::then([] { return 20; }));
            ++progress.slow_steps;
            std::printf("[slow] 1. load: done, value=%d\n", loaded);

            // 2. just + then + let_value：同步转换，再把值传入下一段异步操作。
            std::puts("[slow] checkpoint 2: transform");
            co_await child.suspend_point();
            std::puts("[slow] 2. transform: begin (500ms)");
            const int transformed = co_await (stdexec::just(loaded)
                | stdexec::then([](int value) { return value * 2; })
                | stdexec::let_value([timer](int value) {
                      return exec::schedule_after(timer, 500ms)
                          | stdexec::then([value] { return value + 2; });
                  }));
            ++progress.slow_steps;
            std::printf("[slow] 2. transform: done, value=%d\n", transformed);

            // 3. 条件分支内也可以设置暂停点，单步从当前暂停点推进到下一暂停点。
            if (transformed > 0) {
                std::puts("[slow] checkpoint 3: validate");
                co_await child.suspend_point();
                std::puts("[slow] 3. validate: begin (500ms)");
                co_await exec::schedule_after(timer, 500ms);
                require(transformed % 2 == 0, "expected an even transformed value");
                ++progress.slow_steps;
                std::puts("[slow] 3. validate: done");
            }

            // 4. 内层 when_all：并发等待两份数据，返回值直接解构。
            // 两个纯 sender 组成一个业务阶段；Pause 在阶段边界生效。
            std::puts("[slow] checkpoint 4: fetch two inputs");
            co_await child.suspend_point();
            std::puts("[slow] 4. fetch: begin when_all(amount: 300ms, bonus: 500ms)");
            auto [amount, bonus] = co_await stdexec::when_all(
                exec::schedule_after(timer, 300ms)
                    | stdexec::then([transformed] { return transformed; }),
                exec::schedule_after(timer, 500ms)
                    | stdexec::then([] { return 3; }));
            ++progress.slow_steps;
            std::printf("[slow] 4. fetch: done, inner when_all returned (%d, %d)\n",
                        amount, bonus);

            // 5. 多值 sender + then：汇总上一步的两个结果。
            std::puts("[slow] checkpoint 5: combine");
            co_await child.suspend_point();
            std::puts("[slow] 5. combine: begin (500ms)");
            co_await exec::schedule_after(timer, 500ms);
            const int summary = co_await (stdexec::just(amount, bonus)
                | stdexec::then([](int value, int extra) { return value + extra; }));
            ++progress.slow_steps;
            std::printf("[slow] 5. combine: done, summary=%d\n", summary);

            // 6. let_value：等待异步发布结束后，再沿 sender 链返回结果。
            std::puts("[slow] checkpoint 6: publish");
            co_await child.suspend_point();
            std::puts("[slow] 6. publish: begin (500ms)");
            const int result = co_await (stdexec::just(summary)
                | stdexec::let_value([timer](int value) {
                      return exec::schedule_after(timer, 500ms)
                          | stdexec::then([value] { return value; });
                  }));
            ++progress.slow_steps;
            std::printf("[slow] 6. publish: done, result=%d\n", result);
            std::puts("[slow] done");
            co_return result;
        }(std::move(slow), timer, progress));

    progress.joined = true;
    std::printf("[work] when_all returned: fast=%d, slow=%d\n", cached, published);
    co_await branch.suspend_point(); // 子 frame 已释放，父分支重新承担暂停责任。
    progress.result = co_await (stdexec::just(cached, published)
        | stdexec::then([](int cached_value, int report) { return cached_value + report; }));
    std::printf("[work] finished: result=%d\n", progress.result);
}

stdexec::task<void> control_task(PauseSource& source, exec::timed_thread_scheduler timer,
                               const Progress& progress)
{
    // 完整控制流程；控制器不持有 branch，不参与业务屏障。
    co_await exec::schedule_after(timer, 750ms);
    std::puts("\n[control] command: PAUSE (wait for the next checkpoint)");
    accept(source.request_pause());

    // request_pause 成功不代表已暂停；当前正在 sleep 的业务步仍会完成。
    // Pause 没有异步快照 sender，这里用异步定时等待观察状态。
    auto paused = source.snapshot();
    while (paused.phase != GatePhase::Paused || paused.waiting_branch_count == 0) {
        require(progress.slow_steps < total_steps, "work ended before reaching the pause");
        co_await exec::schedule_after(timer, 10ms);
        paused = source.snapshot();
    }
    const int before_step = progress.slow_steps;
    require(before_step + 1 < total_steps, "not enough work left to demonstrate STEP and CONTINUE");
    require(progress.fast_done && !progress.joined && paused.waiting_branch_count == 1,
            "expected fast completed, slow paused, and when_all still waiting");
    std::printf("[control] PAUSED: fast done, slow paused after %d steps; when_all still waiting\n",
                progress.slow_steps);
    std::puts("[control] hold 1 second\n");

    co_await exec::schedule_after(timer, 1s);
    require(progress.slow_steps == before_step && !progress.joined,
            "work advanced or when_all returned while slow was paused");
    std::puts("[control] command: STEP (run one step, then pause again)\n");
    accept(source.resume(*paused.paused_epoch, AdvanceMode::SingleStep));

    paused = source.snapshot();
    while (paused.phase != GatePhase::Paused || paused.waiting_branch_count == 0) {
        require(progress.slow_steps < total_steps, "work ended before the next pause");
        co_await exec::schedule_after(timer, 10ms);
        paused = source.snapshot();
    }
    require(progress.slow_steps == before_step + 1 && !progress.joined &&
                paused.waiting_branch_count == 1,
            "single step did not execute exactly one step while when_all waited");
    std::printf("[control] PAUSED again: slow has done %d steps; when_all still waiting\n",
                progress.slow_steps);
    std::puts("[control] hold 1 second\n");

    co_await exec::schedule_after(timer, 1s);
    require(progress.slow_steps == before_step + 1 && !progress.joined,
            "work advanced or when_all returned while slow was paused");
    std::puts("[control] command: CONTINUE (run the remaining steps)\n");
    accept(source.resume(*paused.paused_epoch, AdvanceMode::Continuous));
    // 指令发完即可结束；根 when_all 会继续等待 work_task。
}
} // namespace

int main()
{
    std::setvbuf(stdout, nullptr, _IONBF, 0); // 重定向/调试控制台也立即显示暂停前的日志。
    try {
        // 两个 task 在同一个 run_loop 上访问；main 只在 join 后检查。
        Progress progress;
        PauseSource source(AdvanceMode::Continuous, []() noexcept {});
        {
            exec::timed_thread_context clock;
            RunLoopThread executor;
            // 一个根作用域管理控制和业务；error/stopped 会取消另一方并等待收尾。
            const auto completed = stdexec::sync_wait(stdexec::starts_on(
                executor.get_scheduler(), stdexec::when_all(
                    control_task(source, clock.get_scheduler(), progress),
                    work_task(take(source.make_root_branch()), clock.get_scheduler(), progress))));
            require(completed.has_value(), "workflow unexpectedly stopped");
        } // 先 join 执行资源，再释放 Source 和观察状态。
        require_drained(source);
        require(progress.slow_steps == total_steps && progress.fast_done && progress.joined &&
                    progress.result == 55 &&
                    source.snapshot().phase == GatePhase::Open,
                "incomplete workflow");
        std::puts("PASS: six explicit stages, result=55; when_all waited for paused slow task");
    } catch (const PauseFailure& failure) {
        std::fprintf(stderr, "%s (PauseError=%d)\n", failure.what(), static_cast<int>(failure.code));
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "simple_workflow: %s\n", error.what());
        return 1;
    }
}
