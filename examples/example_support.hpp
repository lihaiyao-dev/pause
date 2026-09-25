#ifndef PAUSE_EXAMPLES_SUPPORT_HPP_
#define PAUSE_EXAMPLES_SUPPORT_HPP_

#include "pause.hpp"

#include <exec/scope.hpp>

#include <atomic>
#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <utility>

// 示例宿主的公共辅助设施；不属于 Pause 的公共 API。
namespace pause_examples {
using coop_pause::AdvanceMode;
using coop_pause::GatePhase;
using coop_pause::PauseBranch;
using coop_pause::PauseError;
using coop_pause::PauseSnapshot;
using coop_pause::PauseSource;

inline void require(bool condition, const char* message)
{
    // 示例也在 Release 下验证结果，不能使用会被 NDEBUG 移除的 assert。
    if (!condition) throw std::runtime_error(message);
}

struct PauseFailure : std::runtime_error {
    PauseError code;
    explicit PauseFailure(PauseError error)
        : std::runtime_error("Pause setup/control failed"), code(error) {}
};

template<class T>
T take(std::expected<T, PauseError> result)
{
    // 将控制/创建失败交给宿主异常清理；移动结果以转交 branch 的唯一所有权。
    if (!result) throw PauseFailure(result.error());
    return std::move(*result);
}

inline void accept(std::expected<void, PauseError> result)
{
    if (!result) throw PauseFailure(result.error());
}

struct ChangeSignal {
    std::atomic<std::size_t> sequence{0};

    // 可以在任意通知线程调用；不执行控制命令、日志或阻塞业务。
    void signal() noexcept
    {
        sequence.fetch_add(1, std::memory_order_release);
        sequence.notify_all();
    }
};

inline std::optional<PauseSnapshot> wait_for_barrier(
    std::stop_token quit, ChangeSignal& changed, const PauseSource& source)
{
    for (;;) {
        // 先记录通知序号，再读状态；两者之间的新通知不会被吞掉。
        const auto observed = changed.sequence.load(std::memory_order_acquire);
        if (quit.stop_requested()) return std::nullopt;
        auto snapshot = source.snapshot();
        if (snapshot.phase == GatePhase::Paused && snapshot.waiting_branch_count != 0)
            return snapshot;
        // 空 Paused 不是完成信号，也不应无限地自动 SingleStep 空批次。
        changed.sequence.wait(observed, std::memory_order_acquire);
    }
}

// 主线程 sync_wait；worker 独立驱动 run_loop；controller 只控制状态。
// true 表示 value，false 表示 stopped；普通业务 error 重新抛出。
template<class Controller>
bool run_scope(ChangeSignal& changed, stdexec::task<void> workflow,
               Controller control, bool prestopped = false)
{
    stdexec::run_loop loop;
    // 两种停止源用途不同：workflow_stop 取消业务，controller 的 token 只结束观察循环。
    stdexec::inplace_stop_source workflow_stop;
    if (prestopped) (void)workflow_stop.request_stop();
    bool value = false;
    std::exception_ptr workflow_error, controller_error;
    {
        std::jthread worker([&]() noexcept { loop.run(); });
        // jthread 的 stop_token 不会自动使 run_loop.run() 返回。
        exec::scope_guard finish_worker{[&]() noexcept {
            loop.finish();
            worker.join();
        }};

        std::jthread controller([&](std::stop_token quit) noexcept {
            try {
                control(quit, workflow_stop);
            } catch (...) {
                controller_error = std::current_exception();
                (void)workflow_stop.request_stop();
            }
        });
        exec::scope_guard stop_controller{[&]() noexcept {
            controller.request_stop();
            changed.signal(); // 唤醒 atomic::wait，再 join。
            controller.join();
        }};

        try {
            // starts_on 建立协程的执行上下文；write_env 注入 Pause 所需的 inplace token。
            // sync_wait 阻塞主线程，不能移到负责驱动该 loop 的 worker 上，否则会死锁。
            // 返回 optional 有值表示 value 完成，空表示 stopped；error 会从这里抛出。
            value = stdexec::sync_wait(
                stdexec::starts_on(loop.get_scheduler(), std::move(workflow))
                | stdexec::write_env(stdexec::prop{
                    stdexec::get_stop_token, workflow_stop.get_token()})).has_value();
        } catch (...) {
            workflow_error = std::current_exception();
        }
        // sync_wait 的根 operation/frame 已释放。
        // 先结束 controller（包括 request_stop 的尾部），再 finish/join worker。
        // 外层 Source、通知对象在这整个区间一直存活。
    }
    if (controller_error) std::rethrow_exception(controller_error);
    if (workflow_error) std::rethrow_exception(workflow_error);
    return value;
}

inline void require_drained(const PauseSource& source)
{
    const auto snapshot = source.snapshot();
    require(snapshot.live_branch_count == 0 && snapshot.waiting_branch_count == 0,
            "scope did not release every branch");
    // 这里只是收尾检查；Source 必须活到 run_scope 与任何附加 worker 都完成 join。
}

// 执行上下文有固定数量；任务数由 sender 树决定，不为每个任务创建线程。
// 必须先等待借用 scheduler 的整个 sender 树结束，再析构此宿主。
// 声明在 Source 后面，使 worker 的通知/完成尾部先 join，之后才释放 Source。
class RunLoopThread {
    stdexec::run_loop loop_;
    std::jthread worker_;

public:
    RunLoopThread() : worker_([this]() noexcept { loop_.run(); }) {}
    RunLoopThread(const RunLoopThread&) = delete;
    RunLoopThread& operator=(const RunLoopThread&) = delete;

    ~RunLoopThread()
    {
        loop_.finish();
        worker_.join();
    }

    auto get_scheduler() noexcept { return loop_.get_scheduler(); }
};

} // namespace pause_examples

#endif // PAUSE_EXAMPLES_SUPPORT_HPP_
