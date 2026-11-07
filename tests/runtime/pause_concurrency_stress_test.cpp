#define PAUSE_ENABLE_INDEPENDENT_BRANCH 1
/* 测试项目：pause_concurrency_stress_test
 * 目的: 验证真实 Pause 公共 API 的并发提交与跨 run_loop 的递归分支所有权.
 * 测试项:
 * - Continuous/SingleStep 初始模式下 4 个调用者同时创建 root, 仅一个成功.
 * - 独立分支创建与 request_pause 竞争, 接受先创建成功或暂停后被拒绝两种顺序.
 * - 4 个工作线程上运行深度 3/15 节点的二叉任务树, 覆盖连续, 单步,
 *   叶屏障取消, 业务异常取消兄弟四条路径, 总计 7 个场景族.
 * 关键预期: 创建错误码/通知数/revision 与获胜顺序一致; 任务恢复到所属线程;
 * 子分支先于父分支退役, 所有 15 个节点均退役; 快照不变量成立且最终责任清零.
 * 正常路径完成所有 join, 取消/异常路径不继续 join, 根结果匹配预期.
 * 入口: PAUSE_BUILD_TESTS=ON 时注册同名 CTest.
 *   ctest --test-dir <build> -C <config> -R "^pause_concurrency_stress_test$" --output-on-failure
 *   pause_concurrency_stress_test --iterations 10
 * 参数: iterations 为 1..1000000, 默认每场景族 10 次.
 * 边界: barrier/原子等待建立时序, 不用 sleep; root 完成后仍 join 宿主工作线程.
 * 树深固定, 增加重复次数不扩大递归深度; 竞争获胜次数仅报告, 不强求每次运行
 * 都出现所有线程交错, 也不将重复通过视为穷尽所有交错的证明.
 */
#include "../support/pause_test_notifications.hpp"
#include "pause_sender_test_support.hpp"

#include <array>
#include <barrier>
#include <bit>
#include <cstdint>
#include <optional>
#include <stdexcept>

// Real public-API races and recursive, cross-resource task ownership. No sleep
// establishes an ordering; all resources outlive root completion and worker join.
namespace {
using coop_pause::AdvanceMode;
using coop_pause::GatePhase;
using coop_pause::PauseBranch;
using coop_pause::PauseError;
using coop_pause::PauseSource;

void root_creation_race(AdvanceMode mode)
{
    std::atomic<int> notifications{0};
    PauseSource source(mode, pause_test::notifications([&]() noexcept { ++notifications; }));
    std::array<std::optional<std::expected<PauseBranch, PauseError>>, 4> results;
    std::array<std::jthread, 4> callers;
    std::barrier launch{5};
    for (std::size_t index = 0; index != callers.size(); ++index) {
        callers[index] = std::jthread([&, index] {
            launch.arrive_and_wait();
            results[index].emplace(source.make_root_branch());
        });
    }
    launch.arrive_and_wait();
    for (auto& caller : callers) caller.join();
    int successes = 0;
    for (const auto& result : results) {
        CHECK(result.has_value());
        if (result->has_value()) ++successes;
        else CHECK(result->error() == PauseError::RootAlreadyCreated);
    }
    CHECK(successes == 1 && notifications == (mode == AdvanceMode::SingleStep ? 1 : 0));
    const auto created = source.snapshot();
    CHECK(created.root_created && created.revision == 1);
    CHECK(created.live_branch_count == 1 && created.waiting_branch_count == 0);
    CHECK(created.phase == (mode == AdvanceMode::Continuous ? GatePhase::Open : GatePhase::Collecting));
    for (auto& result : results) result.reset();
    const auto retired = source.snapshot();
    CHECK(retired.revision == 2 && notifications == (mode == AdvanceMode::SingleStep ? 2 : 0));
    CHECK(retired.live_branch_count == 0 && retired.waiting_branch_count == 0);
    CHECK(retired.phase == (mode == AdvanceMode::Continuous ? GatePhase::Open : GatePhase::Paused));
}

bool independent_creation_pause_race()
{
    std::atomic<int> notifications{0};
    PauseSource source(AdvanceMode::Continuous, pause_test::notifications([&]() noexcept { ++notifications; }));
    bool created = false;
    {
        auto root = source.make_root_branch(); CHECK(root);
        std::optional<std::expected<PauseBranch, PauseError>> independent;
        std::barrier launch{3};
        std::jthread creator([&] {
            launch.arrive_and_wait();
            independent.emplace(source.make_branch());
        });
        std::jthread controller([&] {
            launch.arrive_and_wait();
            CHECK(source.request_pause());
        });
        launch.arrive_and_wait();
        creator.join(); controller.join();
        CHECK(independent.has_value());
        created = independent->has_value();
        if (!created) CHECK(independent->error() == PauseError::InvalidBranchState);
        const auto collected = source.snapshot();
        CHECK(collected.phase == GatePhase::Collecting && !collected.paused_epoch);
        CHECK(collected.live_branch_count == (created ? 2u : 1u));
        CHECK(collected.waiting_branch_count == 0 && collected.revision == (created ? 3u : 2u));
        CHECK(notifications == 1);
        independent.reset();
        const auto pending_root = source.snapshot();
        CHECK(pending_root.phase == GatePhase::Collecting && pending_root.live_branch_count == 1);
        CHECK(pending_root.revision == (created ? 4u : 2u));
    }
    const auto empty = source.snapshot();
    CHECK(empty.phase == GatePhase::Paused && empty.paused_epoch);
    CHECK(empty.paused_epoch->value == empty.revision);
    CHECK(empty.revision == (created ? 5u : 3u));
    CHECK(empty.live_branch_count == 0 && empty.waiting_branch_count == 0);
    CHECK(notifications == 2);
    return created;
}

struct Signal {
    std::atomic<std::size_t> sequence{0};
    void notify() noexcept
    {
        sequence.fetch_add(1, std::memory_order_release);
        sequence.notify_all();
    }
};

struct Workers {
    static constexpr std::size_t count = 4;
    std::array<stdexec::run_loop, count> loops;
    std::array<std::thread::id, count> ids;
    std::latch ready{count};
    std::array<std::jthread, count> threads;

    Workers()
    {
        for (std::size_t index = 0; index != count; ++index) {
            threads[index] = std::jthread([&, index] {
                ids[index] = std::this_thread::get_id();
                ready.count_down();
                loops[index].run();
            });
        }
        ready.wait();
    }
    void join() noexcept
    {
        for (auto& loop : loops) loop.finish();
        for (auto& thread : threads) if (thread.joinable()) thread.join();
    }
    ~Workers() { join(); }
    unsigned current_bit() const noexcept
    {
        for (std::size_t index = 0; index != count; ++index)
            if (std::this_thread::get_id() == ids[index]) return 1u << index;
        return 0;
    }
};

enum class Path { Continuous, SingleStep, Cancel, Error };
enum class Outcome { None, Value, Stopped, Error };
struct BusinessFailure : std::runtime_error {
    BusinessFailure() : std::runtime_error("expected recursive business error") {}
};

struct TreeState {
    static constexpr int depth = 3;
    static constexpr int leaf_count = 1 << depth;
    static constexpr int node_count = 2 * leaf_count - 1;
    Workers& workers;
    Path path;
    // Only nodes 1/2 use this barrier, on distinct run_loop threads. All other
    // recursion remains asynchronous, so no worker waits for a queued peer.
    std::barrier<> first_level{2};
    std::array<std::atomic<bool>, node_count> retired{};
    std::atomic<int> entries{0}, forks{0}, joins{0}, retirements{0};
    std::atomic<unsigned> fork_threads{0}, retirement_threads{0};
};

struct Witness {
    TreeState* state;
    int node;
    Witness(TreeState& tree, int index) noexcept : state(&tree), node(index) {}
    Witness(const Witness&) = delete;
    Witness(Witness&& other) noexcept : state(std::exchange(other.state, nullptr)), node(other.node) {}
    ~Witness()
    {
        if (!state) return;
        if (node < TreeState::leaf_count - 1) {
            CHECK(state->retired[2 * node + 1].load(std::memory_order_acquire));
            CHECK(state->retired[2 * node + 2].load(std::memory_order_acquire));
        }
        CHECK(!state->retired[node].exchange(true, std::memory_order_release));
        ++state->retirements;
        state->retirement_threads.fetch_or(state->workers.current_bit(), std::memory_order_relaxed);
    }
};

struct OwnedBranch {
    // Actual branch retirement precedes its witness; children must retire before
    // the parent's frame, including stopped/error unwinding of nested when_all.
    Witness witness;
    PauseBranch branch;
    OwnedBranch(TreeState& state, int node, PauseBranch value) noexcept
        : witness(state, node), branch(std::move(value)) {}
    OwnedBranch(const OwnedBranch&) = delete;
    OwnedBranch(OwnedBranch&&) noexcept = default;
};

stdexec::task<void> recursive_task(OwnedBranch owned, TreeState& state, int node, int depth)
{
    const auto lane = static_cast<std::size_t>(node) % Workers::count;
    CHECK(std::this_thread::get_id() == state.workers.ids[lane]);
    co_await owned.branch.suspend_point({static_cast<std::size_t>(node)});
    CHECK(std::this_thread::get_id() == state.workers.ids[lane]);
    ++state.entries;
    if (depth == 0) {
        if (state.path == Path::Error) {
            if (node == TreeState::leaf_count - 1) throw BusinessFailure{};
            // Error at the first leaf must cancel siblings waiting in this
            // next SingleStep round without any additional controller Resume.
            co_await owned.branch.suspend_point();
            CHECK(false);
        }
        co_return;
    }
    if (node == 1 || node == 2) state.first_level.arrive_and_wait();
    auto children = owned.branch.fork_and_block_self<2>(); CHECK(children);
    ++state.forks;
    state.fork_threads.fetch_or(1u << lane, std::memory_order_relaxed);
    const int left = 2 * node + 1, right = left + 1;
    co_await stdexec::when_all(
        stdexec::starts_on(state.workers.loops[left % Workers::count].get_scheduler(),
            recursive_task(OwnedBranch{state, left, std::move((*children)[0])}, state, left, depth - 1)),
        stdexec::starts_on(state.workers.loops[right % Workers::count].get_scheduler(),
            recursive_task(OwnedBranch{state, right, std::move((*children)[1])}, state, right, depth - 1)));
    CHECK(std::this_thread::get_id() == state.workers.ids[lane]);
    CHECK(state.retired[left].load(std::memory_order_acquire));
    CHECK(state.retired[right].load(std::memory_order_acquire));
    co_await owned.branch.suspend_point();
    CHECK(std::this_thread::get_id() == state.workers.ids[lane]);
    ++state.joins;
}

void check_snapshot(const coop_pause::PauseSnapshot& snapshot)
{
    CHECK(snapshot.waiting_branch_count <= snapshot.live_branch_count);
    CHECK(snapshot.phase != GatePhase::Open || snapshot.waiting_branch_count == 0);
    CHECK((snapshot.phase == GatePhase::Paused) == snapshot.paused_epoch.has_value());
    if (snapshot.phase == GatePhase::Paused) {
        CHECK(snapshot.waiting_branch_count == snapshot.live_branch_count);
        CHECK(snapshot.paused_epoch->value <= snapshot.revision);
    }
    if (snapshot.phase == GatePhase::Collecting)
        CHECK(snapshot.waiting_branch_count < snapshot.live_branch_count);
}

void recursive_scope(Path path)
{
    Signal changed;
    PauseSource source(path == Path::Continuous ? AdvanceMode::Continuous : AdvanceMode::SingleStep, pause_test::notifications([&]() noexcept { changed.notify(); }));
    stdexec::inplace_stop_source stop;
    Workers workers;
    TreeState state{workers, path};
    std::atomic<Outcome> result{Outcome::None};
    auto root = source.make_root_branch(); CHECK(root);
    std::jthread runner([&] {
        Outcome completed;
        try {
            auto value = stdexec::sync_wait(stdexec::write_env(
                stdexec::starts_on(workers.loops[0].get_scheduler(),
                    recursive_task(OwnedBranch{state, 0, std::move(*root)}, state, 0, TreeState::depth)),
                stdexec::prop{stdexec::get_stop_token, stop.get_token()}));
            completed = value ? Outcome::Value : Outcome::Stopped;
        } catch (const BusinessFailure&) {
            completed = Outcome::Error;
        } catch (...) { CHECK(false); completed = Outcome::Error; }
        // sync_wait's root operation and owning frames have already released.
        result.store(completed, std::memory_order_release);
        changed.notify();
    });
    bool terminal_command = false;
    std::size_t previous_revision = 0, resumes = 0;
    while (result.load(std::memory_order_acquire) == Outcome::None) {
        const auto observed = changed.sequence.load(std::memory_order_acquire);
        const auto snapshot = source.snapshot();
        check_snapshot(snapshot);
        CHECK(snapshot.revision >= previous_revision);
        previous_revision = snapshot.revision;
        if (!terminal_command && snapshot.phase == GatePhase::Paused && snapshot.waiting_branch_count != 0) {
            CHECK(path != Path::Continuous);
            const bool leaves = snapshot.waiting_branch_count == TreeState::leaf_count;
            if (path == Path::Cancel && leaves) {
                terminal_command = true;
                CHECK(stop.request_stop());
            } else {
                if (path == Path::Error && leaves) terminal_command = true;
                CHECK(source.resume(*snapshot.paused_epoch, AdvanceMode::SingleStep));
                // Independently finishing subtrees can reenter different
                // epochs. Bound by actual pause operations, not tree levels.
                CHECK(++resumes <= 2 * TreeState::node_count - TreeState::leaf_count);
            }
        } else if (result.load(std::memory_order_acquire) == Outcome::None) {
            changed.sequence.wait(observed, std::memory_order_acquire);
        }
    }
    runner.join();
    // Root completion alone is not a join of start/notifier/callback tails.
    // Keep Source/state/stop alive until each run_loop invocation has returned.
    workers.join();
    const auto final = source.snapshot();
    check_snapshot(final);
    CHECK(final.live_branch_count == 0 && final.waiting_branch_count == 0);
    CHECK(state.forks == TreeState::leaf_count - 1);
    CHECK(state.retirements == TreeState::node_count);
    CHECK(std::popcount(state.fork_threads.load()) >= 2);
    for (const auto& retired : state.retired) CHECK(retired.load());
    if (path == Path::Continuous || path == Path::SingleStep) {
        CHECK(result == Outcome::Value && !terminal_command);
        CHECK(state.entries == TreeState::node_count && state.joins == TreeState::leaf_count - 1);
        CHECK(std::popcount(state.retirement_threads.load()) >= 2);
        if (path == Path::Continuous) CHECK(resumes == 0);
        else CHECK(resumes >= TreeState::depth + 1);
    } else {
        CHECK(terminal_command && state.joins == 0);
        CHECK(result == (path == Path::Cancel ? Outcome::Stopped : Outcome::Error));
        CHECK(resumes == (path == Path::Cancel ? TreeState::depth : TreeState::depth + 1));
        if (path == Path::Cancel) CHECK(state.entries == TreeState::leaf_count - 1);
    }
    CHECK(final.phase == (path == Path::Continuous ? GatePhase::Open : GatePhase::Paused));
}

const char* name(Path path)
{
    switch (path) {
    case Path::Continuous: return "continuous";
    case Path::SingleStep: return "single-step";
    case Path::Cancel: return "cancel-at-leaf-barrier";
    case Path::Error: return "business-error-cancels-siblings";
    }
    std::terminate();
}
} // namespace

int main(int argc, char** argv)
{
    const int count = argc == 1 ? 10 : pause_sender_test::iterations(argc, argv);
    int creation_wins = 0;
    for (int iteration = 0; iteration != count; ++iteration) {
        root_creation_race(AdvanceMode::Continuous);
        root_creation_race(AdvanceMode::SingleStep);
        creation_wins += independent_creation_pause_race();
    }
    std::printf("PASS: root creation 4 callers, %d scopes per initial mode; branch/pause %d scopes "
                "(created=%d rejected=%d)\n", count, count, creation_wins, count - creation_wins);
    std::fflush(stdout);
    for (auto path : {Path::Continuous, Path::SingleStep, Path::Cancel, Path::Error}) {
        for (int iteration = 0; iteration != count; ++iteration) recursive_scope(path);
        std::printf("PASS: recursive path=%s workers=4 depth=%d nodes=%d complete_scopes=%d\n",
                    name(path), TreeState::depth, TreeState::node_count, count);
        std::fflush(stdout);
    }
    std::printf("real Pause concurrency: %d complete scopes, 7 state families, %d repetitions each\n",
                count * 7, count);
}
