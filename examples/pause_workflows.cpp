#include "example_support.hpp"

#include <exec/when_any.hpp>

#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

// 可直接运行的应用示例。辅助设施属于宿主，不是 Pause 的公共 API。
namespace {
using namespace pause_examples;

struct Progress {
    // worker 更新业务进度，controller 在屏障处读取，因此使用原子计数。
    std::atomic<int> units{0};
    std::atomic<int> joins{0};
};

// 案例：导入、转换、发布三个完整业务单元，各单元之前设置安全点。
stdexec::task<void> import_pipeline(PauseBranch branch, Progress& progress)
{
    for (std::size_t stage = 0; stage != 3; ++stage) {
        // tag 仅标记业务位置；不会自动输出日志或成为按位置暂停的断点。
        co_await branch.suspend_point({stage});
        ++progress.units; // 替换为一个保持业务不变量的工作单元。
    }
}

void pipeline_example(bool single_step)
{
    ChangeSignal changed;
    Progress progress;
    // 逆序析构保证 source 先于其通知器借用的 changed 释放。
    // SingleStep 从入口开始收集；Continuous 则由下方 request_pause 主动关门。
    PauseSource source(single_step ? AdvanceMode::SingleStep : AdvanceMode::Continuous,
                       [&]() noexcept { changed.signal(); });
    auto root = take(source.make_root_branch());
    if (!single_step) accept(source.request_pause()); // 入口第一段业务前暂停。
    int steps = 0;
    const bool value = run_scope(changed, import_pipeline(std::move(root), progress),
        [&](std::stop_token quit, auto&) {
            if (single_step) {
                while (auto paused = wait_for_barrier(quit, changed, source)) {
                    require(paused->step_round == static_cast<std::size_t>(steps),
                            "unexpected single-step round");
                    // 一次放行所有当前 waiter，并建立下一轮屏障；不是全局执行一条指令。
                    accept(source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));
                    ++steps;
                }
                return;
            }

            auto first = wait_for_barrier(quit, changed, source);
            if (!first) return;
            require(progress.units == 0, "business ran before the entry barrier");
            accept(source.resume(*first->paused_epoch, AdvanceMode::SingleStep));
            auto next = wait_for_barrier(quit, changed, source);
            if (!next) return;
            require(progress.units == 1, "one step must finish one pipeline unit");
            // 单控制者、已确定处于新 Paused 的本例中，旧 epoch 必然被拒绝。
            auto stale = source.resume(*first->paused_epoch, AdvanceMode::Continuous);
            require(!stale && stale.error() == PauseError::StalePauseEpoch,
                    "stale epoch was accepted");
            accept(source.resume(*next->paused_epoch, AdvanceMode::Continuous));
        });
    require(value && progress.units == 3, "pipeline did not complete three units");
    require(!single_step || steps == 3, "single-step count differs from pause points");
    require_drained(source);
    require(source.snapshot().phase == (single_step ? GatePhase::Paused : GatePhase::Open),
            "unexpected final gate");
}

// 案例：分块计算 / map-reduce。每个协程按值拥有自己的 branch。
stdexec::task<int> reduce_tree(PauseBranch parent, int depth, Progress& progress)
{
    co_await parent.suspend_point();
    ++progress.units;
    if (depth == 0) co_return 1;
    // fork 只登记子责任并阻塞父责任；实际启动与等待由 when_all 负责。
    auto children = take(parent.fork_and_block_self<2>());
    auto [left, right] = co_await stdexec::when_all(
        reduce_tree(std::move(children[0]), depth - 1, progress),
        reduce_tree(std::move(children[1]), depth - 1, progress));
    // when_all 已完成且正常返回的 child frames 已释放；parent 原子重入。
    co_await parent.suspend_point();
    ++progress.joins;
    co_return left + right;
}

stdexec::task<void> tree_workflow(PauseBranch root, Progress& progress, int& result)
{
    result = co_await reduce_tree(std::move(root), 2, progress);
}

void nested_example()
{
    // 深度 2 的二叉树：7 个节点、4 个叶子、3 个父节点汇总。
    ChangeSignal changed;
    Progress progress;
    int result = 0;
    PauseSource source(AdvanceMode::SingleStep, [&]() noexcept { changed.signal(); });
    auto root = take(source.make_root_branch());
    const bool value = run_scope(changed, tree_workflow(std::move(root), progress, result),
        [&](std::stop_token quit, auto&) {
            while (auto paused = wait_for_barrier(quit, changed, source))
                accept(source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));
        });
    require(value && result == 4 && progress.units == 7 && progress.joins == 3,
            "nested map/reduce result or ownership was incorrect");
    require_drained(source);
}

// then 处理同步值变换，let_value 把前一阶段的结果展开成下一个异步阶段。
// branch 属于 task frame；链中的引用只在这次 co_await 内借用它。
stdexec::task<void> sender_pipeline(PauseBranch branch, Progress& progress, int& result)
{
    result = co_await (
        stdexec::just(6) // 待导入的记录数，数据由 sender operation 保存。
        | stdexec::let_value([&](int& records) {
              return branch.suspend_point({0})
                   | stdexec::then([&] {
                         ++progress.units;
                         return records * 2; // 解析成字段。
                     });
          })
        | stdexec::let_value([&](int& fields) {
              return branch.suspend_point({1})
                   | stdexec::then([&] {
                         ++progress.units;
                         return fields + 1; // 添加派生字段。
                     });
          })
        | stdexec::let_value([&](int& fields) {
              return branch.suspend_point({2})
                   | stdexec::then([&] {
                         ++progress.units;
                         return fields; // 发布完整结果。
                     });
          }));
}

void sender_pipeline_example()
{
    ChangeSignal changed;
    Progress progress;
    int result = 0;
    int steps = 0;
    PauseSource source(AdvanceMode::SingleStep, [&]() noexcept { changed.signal(); });
    const bool value = run_scope(changed,
        sender_pipeline(take(source.make_root_branch()), progress, result),
        [&](std::stop_token quit, auto&) {
            while (auto paused = wait_for_barrier(quit, changed, source)) {
                require(progress.units == steps, "pipeline crossed a stage without permission");
                accept(source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));
                ++steps;
            }
        });
    require(value && result == 13 && progress.units == 3 && steps == 3,
            "sender pipeline lost a value or a pause boundary");
    require_drained(source);
}

stdexec::task<void> bulk_batches(PauseBranch branch, Progress& progress, int& result)
{
    for (const std::size_t count : {0u, 3u}) {
        // 一个批次是完整业务单元。bulk 的元素变换不共享/操作 branch。
        result += co_await (branch.suspend_point({count})
            | stdexec::let_value([&] {
                  return stdexec::just(std::array{1, 2, 3})
                       | stdexec::bulk(stdexec::seq, count,
                             [](std::size_t index, auto& values) noexcept { values[index] *= 2; })
                       | stdexec::then([&](auto values) noexcept {
                             ++progress.units;
                             return values[0] + values[1] + values[2];
                         });
              }));
    }
}

void bulk_example()
{
    ChangeSignal changed;
    Progress progress;
    int result = 0;
    int steps = 0;
    PauseSource source(AdvanceMode::SingleStep, [&]() noexcept { changed.signal(); });
    const bool value = run_scope(changed,
        bulk_batches(take(source.make_root_branch()), progress, result),
        [&](std::stop_token quit, auto&) {
            while (auto paused = wait_for_barrier(quit, changed, source)) {
                require(progress.units == steps, "bulk crossed a batch boundary");
                accept(source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));
                ++steps;
            }
        });
    // 零元素 shape 保留原值（和为6），三元素 shape 全部翻倍（和为12）。
    require(value && result == 18 && progress.units == 2 && steps == 2,
            "bulk did not preserve its input or complete each batch");
    require_drained(source);
}

stdexec::task<int> compute_shard(PauseBranch branch, int input, Progress& progress,
                               std::thread::id& thread)
{
    thread = std::this_thread::get_id();
    co_await branch.suspend_point();
    require(std::this_thread::get_id() == thread, "shard resumed on the wrong run_loop");
    ++progress.units;
    const int mapped = input * 2;
    co_await branch.suspend_point();
    require(std::this_thread::get_id() == thread, "shard changed run_loop between stages");
    ++progress.units;
    co_return mapped + 1;
}

stdexec::task<void> parallel_sum(PauseBranch parent, stdexec::run_loop::scheduler other,
                               Progress& progress, std::array<std::thread::id, 2>& threads,
                               int& result)
{
    co_await parent.suspend_point();
    auto children = take(parent.fork_and_block_self<2>());
    auto [left, right] = co_await stdexec::when_all(
        compute_shard(std::move(children[0]), 10, progress, threads[0]),
        stdexec::starts_on(other,
            compute_shard(std::move(children[1]), 20, progress, threads[1])));
    // when_all 等待全部 children；整个 await 语句结束后，child frames 已释放。
    // 父分支重新参与屏障，随后 then 才执行受保护的汇总业务。
    result = co_await (parent.suspend_point()
        | stdexec::then([&] {
              ++progress.joins;
              return left + right;
          }));
}

void parallel_example()
{
    ChangeSignal changed;
    Progress progress;
    std::array<std::thread::id, 2> threads{};
    int result = 0;
    int steps = 0;
    PauseSource source(AdvanceMode::SingleStep, [&]() noexcept { changed.signal(); });
    {
        RunLoopThread other;
        const bool value = run_scope(changed,
            parallel_sum(take(source.make_root_branch()), other.get_scheduler(),
                         progress, threads, result),
            [&](std::stop_token quit, auto&) {
                while (auto paused = wait_for_barrier(quit, changed, source)) {
                    const std::size_t expected = steps == 1 || steps == 2 ? 2 : 1;
                    require(paused->waiting_branch_count == expected,
                            "fork/join did not transfer pause responsibility");
                    require(progress.units == (steps < 2 ? 0 : steps == 2 ? 2 : 4),
                            "parallel stages crossed the group barrier");
                    require(progress.joins == 0, "parent reduced before its join barrier");
                    accept(source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));
                    ++steps;
                }
            });
        require(value && result == 62 && progress.units == 4 && progress.joins == 1 && steps == 4,
                "parallel map/reduce did not join correctly");
        require(threads[0] != threads[1], "shards did not use distinct worker threads");
    } // 第二个执行器也已 finish/join，之后才允许释放 Source。
    require_drained(source);
}

stdexec::task<int> mirror_request(PauseBranch branch, bool slow, Progress& progress)
{
    co_await branch.suspend_point();
    // 模拟需要两个阶段的镜像；第二阶段仍在安全点，能够响应竞速取消。
    if (slow) co_await branch.suspend_point();
    ++progress.units;
    co_return slow ? 99 : 42;
}

stdexec::task<void> first_response(PauseBranch parent, Progress& progress, int& result,
                                 std::atomic<bool>& joined, ChangeSignal& changed)
{
    co_await parent.suspend_point();
    auto children = take(parent.fork_and_block_self<2>());
    result = co_await exec::when_any(
        mirror_request(std::move(children[0]), false, progress),
        mirror_request(std::move(children[1]), true, progress));
    // when_any 请求其余子任务停止并等待全部完成；分号结束后 loser frame 也释放。
    // 它选择首个完成（value/error/stopped），不是“忽略失败直到首个成功”。
    joined.store(true, std::memory_order_release);
    changed.signal();
    co_await parent.suspend_point();
    ++progress.joins;
}

void race_example()
{
    ChangeSignal changed;
    Progress progress;
    int result = 0;
    int steps = 0;
    std::atomic<bool> joined{false};
    PauseSource source(AdvanceMode::SingleStep, [&]() noexcept { changed.signal(); });
    const bool value = run_scope(changed,
        first_response(take(source.make_root_branch()), progress, result, joined, changed),
        [&](std::stop_token quit, auto&) {
            for (; steps != 2; ++steps) {
                auto paused = wait_for_barrier(quit, changed, source);
                if (!paused) return;
                require(paused->waiting_branch_count == (steps == 1 ? 2u : 1u),
                        "race requests did not reach their entry barrier");
                require(progress.units == 0, "race started before the entry barrier");
                accept(source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));
            }
            // winner 退出到 loser 撤责之间可能短暂 Paused；不能把它当作 join 完成，
            // 也不能放行 loser。先观察 sender 完成，再等待 parent 的重入屏障。
            for (;;) {
                const auto observed = changed.sequence.load(std::memory_order_acquire);
                if (quit.stop_requested()) return;
                if (joined.load(std::memory_order_acquire)) break;
                changed.sequence.wait(observed, std::memory_order_acquire);
            }
            auto paused = wait_for_barrier(quit, changed, source);
            if (!paused) return;
            require(paused->waiting_branch_count == 1 && progress.units == 1,
                    "race did not retire the loser before parent reentry");
            accept(source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));
            ++steps;
        });
    require(value && result == 42 && progress.units == 1 && progress.joins == 1 && steps == 3,
            "when_any did not cancel and join the losing request");
    require_drained(source);
}

void cancellation_example(bool prestopped)
{
    ChangeSignal changed;
    Progress progress;
    PauseSource source(AdvanceMode::SingleStep, [&]() noexcept { changed.signal(); });
    auto root = take(source.make_root_branch());
    const bool value = run_scope(changed, import_pipeline(std::move(root), progress),
        [&](std::stop_token quit, auto& workflow_stop) {
            if (prestopped) return;
            if (wait_for_barrier(quit, changed, source)) {
                // 停止由宿主发起；无需也不应通过 resume() 实现取消。
                (void)workflow_stop.request_stop();
            }
        }, prestopped);
    require(!value && progress.units == 0, "cancelled workflow continued business");
    require_drained(source);
}

enum class Reply { Value, Missing, Error };

// 模拟一个业务接口：成功得到值、独立停止、业务错误是三个不同通道。
stdexec::task<int> optional_request(PauseBranch branch, Reply reply)
{
    co_await branch.suspend_point();
    if (reply == Reply::Missing) co_await stdexec::just_stopped();
    if (reply == Reply::Error) throw std::runtime_error("remote validation failed");
    co_return 42;
}

stdexec::task<int> required_request(PauseBranch branch, Progress& progress)
{
    co_await branch.suspend_point();
    // 第二个安全点让本请求能在同组其他请求结束时仍保持暂停。
    co_await branch.suspend_point();
    ++progress.units;
    co_return 7;
}

// 保留 inner 接口时的宿主适配；如果能改业务接口，优先让它直接返回 optional。
stdexec::task<std::optional<int>> as_optional_task(stdexec::task<int> inner)
{
    // 不直接把 stopped_as_optional(inner) 放入外层 when_all：它可能保留 stopped
    // frame 到整组结束，使兄弟等待一个仍登记着责任、却不再到达暂停点的分支。
    auto result = co_await stdexec::stopped_as_optional(std::move(inner));
    // 上一条语句的分号结束临时 awaiter/operation 的生命周期：
    // 即使 inner stopped，它拥有的 frame/branch 也已先行释放。
    co_return result;
}

stdexec::task<void> request_pair(PauseBranch parent, Reply reply, bool keep_sibling,
                                Progress& progress, std::optional<int>& optional_result)
{
    co_await parent.suspend_point();
    auto children = take(parent.fork_and_block_self<2>());
    if (keep_sibling) {
        // 仅把“无结果”的 stopped 转成正常 optional；业务 error 仍会取消兄弟。
        auto [maybe, required] = co_await stdexec::when_all(
            as_optional_task(optional_request(std::move(children[0]), reply)),
            required_request(std::move(children[1]), progress));
        require(required == 7, "required request result was lost");
        optional_result = maybe;
    } else {
        co_await stdexec::when_all(
            // 默认组合语义：任一 child 的 error/stopped 都会向兄弟请求停止。
            optional_request(std::move(children[0]), reply),
            required_request(std::move(children[1]), progress));
    }
    co_await parent.suspend_point();
    ++progress.joins;
}

void requests_example(Reply reply, bool keep_sibling)
{
    ChangeSignal changed;
    Progress progress;
    std::optional<int> optional_result;
    PauseSource source(AdvanceMode::SingleStep, [&]() noexcept { changed.signal(); });
    auto root = take(source.make_root_branch());
    bool value = false;
    bool expected_error = false;
    try {
        value = run_scope(changed,
            request_pair(std::move(root), reply, keep_sibling, progress, optional_result),
            [&](std::stop_token quit, auto&) {
                // 先放行 parent，再放行两个 child 的入口。
                for (int round = 0; round != 2; ++round) {
                    auto paused = wait_for_barrier(quit, changed, source);
                    if (!paused) return;
                    accept(source.resume(*paused->paused_epoch, AdvanceMode::SingleStep));
                }
                if (keep_sibling && reply != Reply::Error) {
                    // A 的 frame/branch 已释放，剩下 B 能独立收敛，不等待 join。
                    auto paused = wait_for_barrier(quit, changed, source);
                    if (!paused) return;
                    require(paused->live_branch_count == 1 && paused->waiting_branch_count == 1,
                            "optional request retained responsibility and blocked its sibling");
                    accept(source.resume(*paused->paused_epoch, AdvanceMode::Continuous));
                }
                // 原样传播 error/stopped 时，不再 resume；when_all 自动取消兄弟。
            });
    } catch (const std::runtime_error& error) {
        if (reply != Reply::Error || std::string_view(error.what()) != "remote validation failed") throw;
        expected_error = true;
    }
    require(expected_error == (reply == Reply::Error), "business error was swallowed");
    const bool normal = keep_sibling && reply != Reply::Error;
    require(value == normal, "unexpected group completion channel");
    require(progress.units == (normal ? 1 : 0) && progress.joins == (normal ? 1 : 0),
            "business continued after group cancellation");
    if (normal) require(optional_result == (reply == Reply::Value ? std::optional{42} : std::nullopt),
                        "optional result mismatch");
    require_drained(source);
}

} // namespace

int main(int argc, char** argv)
{
    struct Example { std::string_view name; void (*run)(); };
    const Example examples[]{
        {"pipeline", [] { pipeline_example(false); }},
        {"single-step", [] { pipeline_example(true); }},
        {"nested", &nested_example},
        {"sender-pipeline", &sender_pipeline_example},
        {"bulk", &bulk_example},
        {"parallel", &parallel_example},
        {"race", &race_example},
        {"cancel", [] { cancellation_example(false); }},
        {"prestop", [] { cancellation_example(true); }},
        {"error", [] { requests_example(Reply::Error, false); }},
        {"stopped", [] { requests_example(Reply::Missing, false); }},
        {"optional", [] { requests_example(Reply::Missing, true); }},
        {"optional-value", [] { requests_example(Reply::Value, true); }},
        {"optional-error", [] { requests_example(Reply::Error, true); }}
    };
    const std::string_view selected = argc == 1 ? "all" : argv[1];
    if (argc > 2) {
        std::fprintf(stderr, "usage: pause_workflows [all|pipeline|single-step|nested|sender-pipeline|bulk|parallel|race|cancel|prestop|error|stopped|optional|optional-value|optional-error]\n");
        return 2;
    }
    bool found = false;
    try {
        for (const auto& example : examples) {
            if (selected != "all" && selected != example.name) continue;
            found = true;
            example.run();
            std::printf("PASS %.*s\n", static_cast<int>(example.name.size()), example.name.data());
        }
    } catch (const PauseFailure& failure) {
        std::fprintf(stderr, "%s (PauseError=%d)\n", failure.what(), static_cast<int>(failure.code));
        return 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "example failed: %s\n", error.what());
        return 1;
    }
    if (!found) { std::fprintf(stderr, "unknown example\n"); return 2; }
}
