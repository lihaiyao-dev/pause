#include "controlled_business.hpp"

#include <exec/timed_thread_scheduler.hpp>

#include <chrono>
#include <cstdio>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace {
using namespace pause_examples;
using namespace pause_controlled;
using namespace std::chrono_literals;

struct Session {
    // 两个 task 都在同一个 run_loop 上访问；宿主在整个作用域 join 后读取。
    bool work_finished = false;
    int barriers = 0;
    std::size_t widest_barrier = 0;
};

struct ControlFailure : std::runtime_error {
    ControlFailure() : std::runtime_error("controller failed at nested barrier") {}
};

// 控制器也是 task：不持有 PauseBranch，不参与业务的暂停责任集合。
stdexec::task<void> control_task(PauseSource& source, exec::timed_thread_scheduler timer,
                               stdexec::inplace_stop_source& stop, Session& session, Mode mode)
{
    const auto thread = std::this_thread::get_id();
    while (!session.work_finished) {
        const auto paused = source.snapshot();
        if (paused.phase == GatePhase::Paused && paused.waiting_branch_count != 0) {
            ++session.barriers;
            if (paused.waiting_branch_count > session.widest_barrier)
                session.widest_barrier = paused.waiting_branch_count;
            std::printf("control: barrier=%d, waiting=%zu\n",
                        session.barriers, paused.waiting_branch_count);

            // 在嵌套任务已展开的屏障上演示取消/失败，无需逐个停止子任务。
            if (paused.waiting_branch_count >= 6) {
                if (mode == Mode::ControlError) throw ControlFailure{};
                if (mode == Mode::Cancel) {
                    std::puts("control: cancel the whole task tree");
                    (void)stop.request_stop();
                    co_return;
                }
            }

            const auto advance = mode == Mode::Continue && session.barriers == 3
                ? AdvanceMode::Continuous : AdvanceMode::SingleStep;
            std::puts(advance == AdvanceMode::SingleStep
                          ? "control: single step" : "control: continue");
            accept(source.resume(*paused.paused_epoch, advance));
        }

        // Pause 当前没有“异步等待快照”的 sender；用随 stdexec 提供的定时 sender
        // 演示异步观察。没有 atomic::wait、sleep 或专门的控制线程阻塞业务执行器。
        // 默认 task 的 affinity 会在定时完成/取消后回到启动它的 run_loop。
        co_await exec::schedule_after(timer, 1ms);
        require(std::this_thread::get_id() == thread, "controller left its run_loop");
    }
    std::puts("control: work completed, leaving the scope");
}

stdexec::task<int> observe_work(stdexec::task<int> work, Session& session)
{
    const int result = co_await std::move(work);
    // 上一句 await 已释放业务 task frame 及其 root branch；完成依据来自 task。
    // 空 Paused/live_branch_count==0 不能代替这个完成信号。
    session.work_finished = true;
    co_return result;
    // error/stopped 原样传播给 when_all，由它取消仍在等待定时器的 control_task。
}

void run_example(Mode mode)
{
    Progress progress;
    Session session;
    stdexec::inplace_stop_source stop;
    // 从入口单步收集，两个 task 的启动先后不影响首个业务安全点。
    // 本例定时读取 snapshot，通知器保留为空操作；生产应用可对接自己的事件循环。
    PauseSource source(AdvanceMode::SingleStep, []() noexcept {});
    bool value = false;
    bool expected_error = false;
    int result = 0;
    {
        exec::timed_thread_context clock;
        RunLoopThread executor;
        try {
            // 全应用只有一次 sync_wait；when_all 同时管理控制和业务两个 task。
            // scheduler 负责执行，fork 负责转交暂停责任，await/when_all 负责等待子任务。
            auto completed = stdexec::sync_wait(
                stdexec::starts_on(executor.get_scheduler(), stdexec::when_all(
                    control_task(source, clock.get_scheduler(), stop, session, mode),
                    observe_work(work_task(take(source.make_root_branch()), progress, mode),
                                 session)))
                | stdexec::write_env(stdexec::prop{stdexec::get_stop_token, stop.get_token()}));
            value = completed.has_value();
            if (completed) result = std::get<0>(*completed);
        } catch (const WorkFailure&) {
            if (mode != Mode::WorkError) throw;
            expected_error = true;
        } catch (const ControlFailure&) {
            if (mode != Mode::ControlError) throw;
            expected_error = true;
        }
        // 即使 error/stopped，when_all 也已等待双方结束。现在才能关闭执行资源。
    } // run_loop 和 timer 线程都已 join；Source、stop、观察状态仍存活。
    require_drained(source);

    const bool failing = mode == Mode::WorkError || mode == Mode::ControlError;
    const bool normal = !failing && mode != Mode::Cancel;
    require(expected_error == failing && value == normal, "unexpected root completion channel");
    require(session.work_finished == normal, "work completion was reported too early");
    if (normal) {
        const bool optional = mode != Mode::SkipOptional;
        require(result == (optional ? 59 : 54) && progress.units == (optional ? 16 : 15),
                "workflow result or business count was incorrect");
        require(progress.optional_ran == optional && progress.forks == 12 && progress.joins == 12,
                "conditional/fork/join workflow was incomplete");
        if (mode == Mode::Continue) {
            require(session.barriers == 3 && source.snapshot().phase == GatePhase::Open,
                    "continuous mode kept pausing the workflow");
        } else {
            require(session.barriers == (optional ? 13 : 12) && session.widest_barrier == 12,
                    "conditional/nested tasks did not share the expected pause barriers");
        }
        std::printf("result=%d, units=%d, forks=%d, joins=%d\n",
                    result, progress.units, progress.forks, progress.joins);
    } else {
        require(session.widest_barrier >= 6, "failure did not exercise nested task cleanup");
        std::printf("root=%s, responsibilities=0\n", expected_error ? "error" : "stopped");
    }
}
} // namespace

int main(int argc, char** argv)
{
    struct Example { std::string_view name; Mode mode; };
    const Example examples[]{
        {"step", Mode::Step}, {"continue", Mode::Continue}, {"skip-optional", Mode::SkipOptional},
        {"cancel", Mode::Cancel}, {"work-error", Mode::WorkError}, {"control-error", Mode::ControlError}};
    const std::string_view selected = argc == 1 ? "step" : argv[1];
    if (argc > 2) {
        std::fputs("usage: pause_controlled_workflow [all|step|continue|skip-optional|cancel|work-error|control-error]\n",
                   stderr);
        return 2;
    }
    bool found = false;
    try {
        for (const auto& example : examples) {
            if (selected != "all" && selected != example.name) continue;
            found = true;
            run_example(example.mode);
            std::printf("PASS %.*s\n", static_cast<int>(example.name.size()), example.name.data());
        }
    } catch (const PauseFailure& failure) {
        std::fprintf(stderr, "%s (PauseError=%d)\n", failure.what(), static_cast<int>(failure.code));
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "controlled_workflow: %s\n", error.what());
        return 1;
    }
    if (!found) { std::fputs("unknown example\n", stderr); return 2; }
}
