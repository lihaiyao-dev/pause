#include "example_support.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <cstdio>
#include <string_view>
#include <thread>
#include <utility>

namespace {
using namespace pause_examples;

struct IndexResult {
    std::size_t jobs = 0;
    std::size_t documents = 0;
    std::size_t checksum = 0;
    std::thread::id first_thread;
    bool used_both_threads = false;
};

IndexResult combine(IndexResult left, IndexResult right)
{
    return {left.jobs + right.jobs, left.documents + right.documents,
            left.checksum + right.checksum, left.first_thread,
            left.used_both_threads || right.used_both_threads ||
                left.first_thread != right.first_thread};
}

enum class FirstOutcome { Pending, Value, Stopped };
enum class CancellationMode { FirstPartition, WholeTree, ControllerError };

struct ControllerFailure : std::runtime_error {
    ControllerFailure() : std::runtime_error("controller failed at the entry barrier") {}
};

struct Cancellation {
    stdexec::inplace_stop_source first_stop;
    std::atomic<FirstOutcome> first_outcome{FirstOutcome::Pending};
    ChangeSignal& changed;
};

// 每个分区协程按值持有 branch；业务结果沿 sender 树返回，无需任务/branch 容器。
stdexec::task<IndexResult> index_partition(PauseBranch branch)
{
    constexpr std::array document_sizes{4, 7, 11};
    IndexResult result;
    result.first_thread = std::this_thread::get_id();
    for (std::size_t i = 0; i != document_sizes.size(); ++i) {
        // 第一条业务也有安全点；一次恢复不会把业务切换到 controller 线程。
        co_await branch.suspend_point({i});
        require(std::this_thread::get_id() == result.first_thread,
                "Continuation ran outside its own run_loop");
        ++result.documents;
        result.checksum += document_sizes[i];
    }
    result.jobs = 1;
    co_return result;
}

stdexec::task<IndexResult> run_partition(PauseBranch branch, std::size_t index,
                                        Cancellation& cancellation)
{
    if (index != 0)
        co_return co_await index_partition(std::move(branch));

    const auto thread = std::this_thread::get_id();
    const auto parent_stop = co_await stdexec::read_env(stdexec::get_stop_token);
    // first_stop 只隔离主动取消的范围；整组失败/取消仍必须传进这个分区，
    // 否则 when_all 清理时可能永远等着一个仍在暂停的 child。
    auto forward_stop = [&]() noexcept { (void)cancellation.first_stop.request_stop(); };
    stdexec::inplace_stop_callback<decltype(forward_stop)> forward{parent_stop, forward_stop};
    auto result = co_await stdexec::stopped_as_optional(
        index_partition(std::move(branch))
        | stdexec::write_env(stdexec::prop{
            stdexec::get_stop_token, cancellation.first_stop.get_token()}));
    // 分号已销毁临时 awaiter/operation 和 stopped 的 inner frame/branch。
    // 必须在这个 task 边界释放它，再把正常 optional 结果交给外层 when_all；
    // 直接组合 stopped_as_optional(task) 可能把 frame 留到整组 join 才销毁。
    cancellation.first_outcome.store(result ? FirstOutcome::Value : FirstOutcome::Stopped,
                                     std::memory_order_release);
    cancellation.changed.signal();
    if (result) co_return *result;
    co_return IndexResult{0, 0, 0, thread, false};
}

stdexec::task<IndexResult> partition_tree(PauseBranch parent, std::size_t first,
                                         std::size_t count,
                                         stdexec::run_loop::scheduler second_scheduler,
                                         Cancellation& cancellation)
{
    if (count == 1) {
        if (first % 2 != 0)
            co_return co_await stdexec::starts_on(second_scheduler,
                run_partition(std::move(parent), first, cancellation));
        co_return co_await run_partition(std::move(parent), first, cancellation);
    }

    // 运行期任务数通过递归二分表达：每层固定 fork<2> + when_all，
    // 不调用返回 vector 的 fork(N)，也不把 branch 放进宿主管理的容器。
    // 拓展任务树不执行业务；所有 leaf 的入口安全点共同形成首轮屏障。
    auto children = take(parent.fork_and_block_self<2>());
    const auto left_count = count / 2;
    auto [left, right] = co_await stdexec::when_all(
        partition_tree(std::move(children[0]), first, left_count,
                       second_scheduler, cancellation),
        partition_tree(std::move(children[1]), first + left_count, count - left_count,
                       second_scheduler, cancellation));
    // join 后子协程已释放 branch；父分支先重新进入责任集合，再进行汇总业务。
    co_await parent.suspend_point();
    co_return combine(left, right);
}

stdexec::task<void> independent_workflow(PauseBranch first, PauseBranch second,
                                        stdexec::run_loop::scheduler second_scheduler,
                                        Cancellation& cancellation, IndexResult& result)
{
    // starts_on 为第二项建立独立执行上下文；when_all 同时启动并等待两个任务。
    auto [left, right] = co_await stdexec::when_all(
        run_partition(std::move(first), 0, cancellation),
        stdexec::starts_on(second_scheduler,
            run_partition(std::move(second), 1, cancellation)));
    result = combine(left, right);
}

stdexec::task<void> dynamic_workflow(PauseBranch root, std::size_t count,
                                    stdexec::run_loop::scheduler second_scheduler,
                                    Cancellation& cancellation, IndexResult& result)
{
    result = co_await partition_tree(std::move(root), 0, count, second_scheduler, cancellation);
}

void run_example(std::size_t count, bool dynamic_fork,
                 CancellationMode mode = CancellationMode::FirstPartition)
{
    ChangeSignal changed;
    Cancellation cancellation{{}, FirstOutcome::Pending, changed};
    PauseSource source{AdvanceMode::Continuous, [&]() noexcept { changed.signal(); }};
    auto root = take(source.make_root_branch());
    IndexResult result;
    // run_scope 自带一个 run_loop；这里只补第二个。2..32 个分区始终共用这两个。
    RunLoopThread second_worker;

    auto control = [&](std::stop_token quit, auto& workflow_stop) {
        auto paused = wait_for_barrier(quit, changed, source);
        if (!paused) return;
        require(paused->paused_epoch.has_value(), "Paused without an epoch");
        require(paused->waiting_branch_count == count && paused->live_branch_count == count,
                "Not all partitions reached the entry barrier");
        const auto epoch = *paused->paused_epoch;

        // 这两条失败路径都不调用 resume；根取消必须穿过局部 token 到达首个分区。
        if (mode == CancellationMode::ControllerError) throw ControllerFailure{};
        if (mode == CancellationMode::WholeTree) {
            (void)workflow_stop.request_stop();
            return;
        }

        // 共享 PauseSource 不代表共享主动取消；首个分区的 stopped 被局部转成空结果。
        (void)cancellation.first_stop.request_stop();
        for (;;) {
            const auto observed = changed.sequence.load(std::memory_order_acquire);
            if (quit.stop_requested()) return;
            if (cancellation.first_outcome.load(std::memory_order_acquire) != FirstOutcome::Pending)
                break;
            changed.sequence.wait(observed, std::memory_order_acquire);
        }
        require(cancellation.first_outcome.load(std::memory_order_acquire) == FirstOutcome::Stopped,
                "Cancelled partition must stop before indexing any document");
        const auto remaining = source.snapshot();
        require(remaining.phase == GatePhase::Paused && remaining.paused_epoch == epoch &&
                    remaining.waiting_branch_count == count - 1 &&
                    remaining.live_branch_count == count - 1,
                "Cancelling one partition must preserve the other waiters and epoch");
        accept(source.resume(epoch, AdvanceMode::Continuous));
        // resume 只放行；顶层 sync_wait 要等整棵 when_all 树完成才会返回。
    };

    bool value = false;
    bool controller_failed = false;
    try {
        if (dynamic_fork) {
            accept(source.request_pause());
            value = run_scope(changed,
                dynamic_workflow(std::move(root), count, second_worker.get_scheduler(),
                                 cancellation, result), control);
        } else {
            // 独立顶层分区只在 Open 中登记；它们不需要伪造共同父分支。
            auto second = take(source.make_branch());
            accept(source.request_pause());
            value = run_scope(changed,
                independent_workflow(std::move(root), std::move(second),
                                     second_worker.get_scheduler(), cancellation, result), control);
        }
    } catch (const ControllerFailure&) {
        // run_scope 先取消、等待全部分区退出，再把控制错误重新抛回宿主。
        if (mode != CancellationMode::ControllerError) throw;
        controller_failed = true;
    }
    if (mode != CancellationMode::FirstPartition) {
        require(!value && controller_failed == (mode == CancellationMode::ControllerError),
                "Root cancellation or controller error was not propagated");
        require_drained(source);
        std::printf("%s: jobs=%zu, stopped=%zu, responsibilities=0\n",
                    controller_failed ? "controller error" : "cancel all", count, count);
        return;
    }
    require(value && result.jobs == count - 1 && result.documents == 3 * (count - 1) &&
                result.checksum == 22 * (count - 1),
            "Resumed partitions did not finish indexing");
    require(result.used_both_threads, "Partitions did not use both run_loop threads");
    require_drained(source);
    std::printf("%s: jobs=%zu, stopped=1, value=%zu, indexed=%zu, responsibilities=0\n",
                dynamic_fork ? "dynamic fork" : "independent jobs", count,
                result.jobs, result.documents);
}

} // namespace

int main(int argc, char** argv)
{
    try {
        if (argc == 2 && std::string_view{argv[1]} == "--cancel-all") {
            run_example(3, true, CancellationMode::WholeTree);
            return 0;
        }
        if (argc == 2 && std::string_view{argv[1]} == "--controller-error") {
            run_example(3, true, CancellationMode::ControllerError);
            return 0;
        }
        std::size_t count = 2;
        const bool dynamic_fork = argc == 3 && std::string_view{argv[1]} == "--dynamic";
        if (argc != 1) {
            require(dynamic_fork,
                    "Usage: pause_independent_jobs [--dynamic N | --cancel-all | --controller-error], N=2..32");
            const std::string_view text{argv[2]};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), count);
            require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() &&
                        count >= 2 && count <= 32,
                    "N must be 2..32 (this example's task limit, not a Pause limit)");
        }
        run_example(count, dynamic_fork);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "independent_jobs: %s\n", error.what());
        return 1;
    }
}
