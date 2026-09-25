#ifndef PAUSE_EXAMPLES_CONTROLLED_BUSINESS_HPP_
#define PAUSE_EXAMPLES_CONTROLLED_BUSINESS_HPP_

#include "example_support.hpp"

#include <cstdio>
#include <stdexcept>
#include <utility>

namespace pause_controlled {
using pause_examples::PauseBranch;
using pause_examples::take;

enum class Mode { Step, Continue, SkipOptional, Cancel, WorkError, ControlError };

struct Progress {
    // control_task 与整个业务树运行在同一个 run_loop 上；宿主仅在 join 后读取。
    int units = 0;
    int forks = 0;
    int joins = 0;
    bool optional_ran = false;
};

struct WorkFailure : std::runtime_error {
    WorkFailure() : std::runtime_error("nested business failed") {}
};

inline stdexec::task<int> load_source(PauseBranch branch, Progress& progress)
{
    co_await branch.suspend_point();
    std::printf("work: load source\n");
    ++progress.units;
    co_return 10;
}

inline stdexec::task<int> prepare_data(PauseBranch branch, int source, Progress& progress)
{
    co_await branch.suspend_point();
    std::printf("work: prepare data from %d\n", source);
    ++progress.units;
    co_return source * 2;
}

inline stdexec::task<int> compute_shard(PauseBranch branch, int seed, int depth,
                                       Progress& progress, bool fail_leaf = false)
{
    co_await branch.suspend_point();
    if (depth == 0) {
        if (fail_leaf) throw WorkFailure{};
        ++progress.units;
        co_return seed;
    }

    auto [left, right] = take(branch.fork_and_block_self<2>());
    ++progress.forks;
    auto [a, b] = co_await stdexec::when_all(
        compute_shard(std::move(left), seed, depth - 1, progress, fail_leaf),
        compute_shard(std::move(right), seed, depth - 1, progress));
    // when_all 等待子树且释放 child frames；父 branch 在此重新加入暂停屏障。
    co_await branch.suspend_point();
    ++progress.joins;
    co_return a + b;
}

inline stdexec::task<int> work_task(PauseBranch branch, Progress& progress, Mode mode)
{
    co_await branch.suspend_point();
    std::printf("work: begin controlled workflow\n");

    auto [load_branch] = take(branch.fork_and_block_self<1>());
    ++progress.forks;
    const int source = co_await load_source(std::move(load_branch), progress);
    // 子 task 拥有唯一的 child branch，await 完成后先恢复父责任，再继续业务。
    co_await branch.suspend_point();
    ++progress.joins;

    int bonus = 0;
    if (mode != Mode::SkipOptional) {
        co_await branch.suspend_point();
        std::puts("work: optional validation");
        progress.optional_ran = true;
        ++progress.units;
        bonus = 5;
    }

    co_await branch.suspend_point();
    auto [prepare_branch] = take(branch.fork_and_block_self<1>());
    ++progress.forks;
    const int prepared = co_await prepare_data(std::move(prepare_branch), source, progress);
    co_await branch.suspend_point();
    ++progress.joins;

    std::puts("work: start three concurrent shards, each with nested children");
    auto [a, b, c] = take(branch.fork_and_block_self<3>());
    ++progress.forks;
    auto [first, second, third] = co_await stdexec::when_all(
        compute_shard(std::move(a), 1, 2, progress),
        compute_shard(std::move(b), 2, 2, progress, mode == Mode::WorkError),
        compute_shard(std::move(c), 3, 2, progress));

    co_await branch.suspend_point();
    ++progress.joins;
    ++progress.units;
    const int result = source + prepared + bonus + first + second + third;
    std::printf("work: joined all shards; result=%d\n", result);
    co_return result;
}

} // namespace pause_controlled

#endif // PAUSE_EXAMPLES_CONTROLLED_BUSINESS_HPP_
