/* 测试项目：pause_exception_matrix_test
 * 目的: 验证真实 Pause 的状态错误, 分支移动, 并发提交期间分配失败的回滚语义.
 * 测试项:
 * - 10 个普通项: root/嵌套父分支在三种 gate 状态下移动; Collecting 中错误
 *   epoch 恢复; Paused 中创建独立分支; root 退役后重建; 单步轮次重启.
 * - 19 个并发 OOM 项: root 创建的两种初始模式; 独立分支分配失败时并发 root
 *   提交/同级创建/暂停; 动态 fork 的 reserve 及固定/动态 fork 三个子记录的
 *   分配点失败, 分别覆盖 Open/Collecting 并让同级 fork 提交.
 * 关键预期: 移动不分配且保留父子关联; 状态拒绝不改快照; OOM 返回
 * ResourceExhausted, 不回滚其他线程已经提交的状态, 通知数不额外增加;
 * 解除故障后重试遵循当前状态, 每个完整作用域的分配余额恢复基线.
 * 入口: PAUSE_BUILD_TESTS=ON 时注册同名 CTest.
 *   ctest --test-dir <build> -C <config> -R "^pause_exception_matrix_test$" --output-on-failure
 *   pause_exception_matrix_test --iterations 2 --ordinary-iterations 2
 * 参数: 两个次数均为 1..1000000; iterations 默认 2, ordinary-iterations 默认
 * 跟随 iterations, 分别控制 19 个并发项和 10 个普通项.
 * 边界: 本可执行文件替换 new/delete, 仅对选定 API 调用线程注入 bad_alloc;
 * 不耗尽系统内存, 不在宿主线程/调度器初始化时注入. MSVC Debug 的 noexcept
 * 迭代器代理分配点不在本矩阵中注入, 其终止行为由独立资源死亡测试覆盖.
 */
#include "pause_sender_test_support.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <vector>
#ifdef _WIN32
#include <malloc.h>
#endif

// This executable alone replaces allocation. Only the thread currently inside
// a selected public API call can fail; thread/scheduler setup is never armed.
namespace exception_allocation_probe {
thread_local std::ptrdiff_t fail_at = -1;
thread_local std::size_t attempts = 0;
thread_local bool injected = false;
thread_local pause_sender_test::Gate* failure_gate = nullptr;
std::atomic<std::ptrdiff_t> outstanding{0};

void before_allocate()
{
    if (fail_at < 0 || attempts++ != static_cast<std::size_t>(fail_at)) return;
    injected = true;
    fail_at = -1; // Synchronization/runtime work cannot recursively trigger OOM.
    if (failure_gate) failure_gate->hold();
    throw std::bad_alloc{};
}

struct Failure {
    explicit Failure(std::size_t index, pause_sender_test::Gate* gate = nullptr) noexcept
    {
        attempts = 0;
        injected = false;
        failure_gate = gate;
        fail_at = static_cast<std::ptrdiff_t>(index);
    }
    ~Failure() { fail_at = -1; failure_gate = nullptr; }
};
} // namespace exception_allocation_probe

void* operator new(std::size_t size)
{
    exception_allocation_probe::before_allocate();
    if (void* pointer = std::malloc(size ? size : 1)) {
        ++exception_allocation_probe::outstanding;
        return pointer;
    }
    throw std::bad_alloc{};
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept
{
    if (pointer) {
        --exception_allocation_probe::outstanding;
        std::free(pointer);
    }
}
void operator delete[](void* pointer) noexcept { ::operator delete(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    exception_allocation_probe::before_allocate();
    const auto align = static_cast<std::size_t>(alignment);
#ifdef _WIN32
    void* pointer = _aligned_malloc(size ? size : 1, align);
#else
    void* pointer = nullptr;
    if (posix_memalign(&pointer, align, size ? size : 1) != 0) pointer = nullptr;
#endif
    if (!pointer) throw std::bad_alloc{};
    ++exception_allocation_probe::outstanding;
    return pointer;
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{ return ::operator new(size, alignment); }
void operator delete(void* pointer, std::align_val_t) noexcept
{
    if (!pointer) return;
    --exception_allocation_probe::outstanding;
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept
{ ::operator delete(pointer, alignment); }
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept
{ ::operator delete(pointer, alignment); }
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept
{ ::operator delete(pointer, alignment); }

namespace {
using namespace pause_sender_test;
using coop_pause::AdvanceMode;
using coop_pause::GatePhase;
using coop_pause::PauseBranch;
using coop_pause::PauseError;
using coop_pause::PauseSource;
using BranchResult = std::expected<PauseBranch, PauseError>;
using Sender = decltype(std::declval<PauseBranch&>().suspend_point());
using Operation = stdexec::connect_result_t<Sender, Receiver>;
const char* current_id = "startup";
int current_iteration = 0;

[[noreturn]] void matrix_fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed: id=%s iteration=%d line=%d: %s\n",
                 current_id, current_iteration, line, expression);
    std::exit(EXIT_FAILURE);
}
#undef CHECK
#define CHECK(expression) ((expression) ? (void)0 : matrix_fail(#expression, __LINE__))

void same_snapshot(const coop_pause::PauseSnapshot& left, const coop_pause::PauseSnapshot& right)
{
    CHECK(left.revision == right.revision && left.root_created == right.root_created);
    CHECK(left.phase == right.phase && left.paused_epoch == right.paused_epoch);
    CHECK(left.origin == right.origin && left.step_round == right.step_round);
    CHECK(left.waiting_branch_count == right.waiting_branch_count);
    CHECK(left.live_branch_count == right.live_branch_count);
}

void moved_delegated_parent(bool nested, GatePhase gate)
{
    std::atomic<std::size_t> notifications{0};
    PauseSource source(AdvanceMode::Continuous, [&]() noexcept { ++notifications; });
    SchedulerState scheduler;
    std::optional<PauseBranch> root, parent, moved_parent;
    {
        auto created = source.make_root_branch(); CHECK(created);
        root.emplace(std::move(*created));
    }
    auto* delegated = &*root;
    if (nested) {
        auto child = root->fork_and_block_self<1>(); CHECK(child);
        parent.emplace(std::move((*child)[0]));
        delegated = &*parent;
    }
    std::array<std::optional<PauseBranch>, 2> children;
    {
        auto fork = delegated->fork_and_block_self<2>(); CHECK(fork);
        children[0].emplace(std::move((*fork)[0]));
        children[1].emplace(std::move((*fork)[1]));
    }
    if (gate != GatePhase::Open) CHECK(source.request_pause());
    stdexec::inplace_stop_source stopped;
    if (gate == GatePhase::Paused) CHECK(!stopped.request_stop());
    ReceiverState resumed{&scheduler, {}};
    std::unique_ptr<Operation> operation;
    run_in_context(scheduler, [&] {
        if (gate == GatePhase::Paused) {
            // Cancel responsibility, but deliberately retain both child records.
            // Their completed operations are destroyed before the parent move.
            for (auto& child : children) {
                ReceiverState receiver{&scheduler, stopped.get_token()};
                receiver.finish_loop = false;
                auto cancelled = stdexec::connect(child->suspend_point(), Receiver{&receiver});
                stdexec::start(cancelled);
                CHECK(receiver.result == Completion::Stopped);
            }
        }
        const auto before = source.snapshot();
        CHECK(before.phase == gate && before.waiting_branch_count == 0);
        CHECK(before.live_branch_count == (gate == GatePhase::Paused ? 0u : 2u));
        const auto before_notifications = notifications.load();
        {
            exception_allocation_probe::Failure forbid_allocation(0);
            moved_parent.emplace(std::move(*delegated));
            if (nested) parent.reset(); else root.reset();
            CHECK(exception_allocation_probe::attempts == 0);
            CHECK(!exception_allocation_probe::injected);
        }
        same_snapshot(source.snapshot(), before);
        CHECK(notifications == before_notifications);
        // Children keep their parent links across the move and source-holder
        // destruction. Retiring them must update the moved record, not its owner.
        children[0].reset();
        children[1].reset();
        const auto retired = source.snapshot();
        CHECK(retired.live_branch_count == 0 && retired.waiting_branch_count == 0);
        CHECK(retired.phase == (gate == GatePhase::Open ? GatePhase::Open : GatePhase::Paused));
        if (gate == GatePhase::Paused) CHECK(retired.paused_epoch == before.paused_epoch);
        auto premature_fork = moved_parent->fork_and_block_self<1>();
        CHECK(!premature_fork && premature_fork.error() == PauseError::InvalidBranchState);
        same_snapshot(source.snapshot(), retired);
        operation.reset(new Operation(stdexec::connect(moved_parent->suspend_point(), Receiver{&resumed})));
        resumed.start_active = true;
        stdexec::start(*operation);
        resumed.start_active = false;
        const auto reentered = source.snapshot();
        CHECK(reentered.live_branch_count == 1 && reentered.revision == retired.revision + 1);
        if (gate == GatePhase::Open) {
            CHECK(resumed.result == Completion::Value && resumed.completed_in_start);
            CHECK(reentered.phase == GatePhase::Open && reentered.waiting_branch_count == 0);
        } else {
            CHECK(resumed.result == Completion::None);
            CHECK(reentered.phase == GatePhase::Paused && reentered.waiting_branch_count == 1);
            CHECK(reentered.paused_epoch == retired.paused_epoch);
            CHECK(source.resume(*reentered.paused_epoch, AdvanceMode::Continuous));
            CHECK(resumed.result == Completion::None);
        }
    });
    operation.reset();
    CHECK(resumed.result == Completion::Value);
    CHECK(scheduler.calls == (gate == GatePhase::Open ? 0 : 1));
    CHECK(scheduler.starts == (gate == GatePhase::Open ? 0 : 1));
    moved_parent.reset();
    root.reset(); // The nested parent retired before its own root record.
    const auto final = source.snapshot();
    CHECK(final.phase == GatePhase::Open && final.live_branch_count == 0 && final.waiting_branch_count == 0);
    CHECK(notifications == final.revision);
}

void resume_collecting_wrong_epoch()
{
    PauseSource source(AdvanceMode::SingleStep, []() noexcept {});
    auto root = source.make_root_branch(); CHECK(root);
    const auto before = source.snapshot();
    CHECK(before.phase == GatePhase::Collecting);
    auto result = source.resume({std::numeric_limits<std::size_t>::max()}, AdvanceMode::Continuous);
    CHECK(!result && result.error() == PauseError::NotPaused);
    same_snapshot(source.snapshot(), before);
}

void make_branch_paused()
{
    PauseSource source(AdvanceMode::SingleStep, []() noexcept {});
    { auto root = source.make_root_branch(); CHECK(root); }
    const auto before = source.snapshot();
    CHECK(before.phase == GatePhase::Paused && before.live_branch_count == 0);
    auto branch = source.make_branch();
    CHECK(!branch && branch.error() == PauseError::InvalidBranchState);
    same_snapshot(source.snapshot(), before);
}

void root_recreate_after_retirement()
{
    PauseSource source(AdvanceMode::Continuous, []() noexcept {});
    { auto root = source.make_root_branch(); CHECK(root); }
    const auto before = source.snapshot();
    CHECK(before.root_created && before.live_branch_count == 0);
    auto root = source.make_root_branch();
    CHECK(!root && root.error() == PauseError::RootAlreadyCreated);
    same_snapshot(source.snapshot(), before);
}

void step_round_restarts()
{
    PauseSource source(AdvanceMode::Continuous, []() noexcept {});
    { auto root = source.make_root_branch(); CHECK(root); }
    for (int cycle = 0; cycle != 2; ++cycle) {
        CHECK(source.request_pause());
        const auto control = source.snapshot();
        CHECK(control.phase == GatePhase::Paused && !control.step_round);
        CHECK(control.origin == coop_pause::PauseOrigin::ControlRequest);
        CHECK(source.resume(*control.paused_epoch, AdvanceMode::SingleStep));
        const auto step = source.snapshot();
        CHECK(step.phase == GatePhase::Paused && step.step_round == 1);
        CHECK(step.origin == coop_pause::PauseOrigin::SingleStep);
        CHECK(step.paused_epoch != control.paused_epoch);
        CHECK(source.resume(*step.paused_epoch, AdvanceMode::Continuous));
        const auto open = source.snapshot();
        CHECK(open.phase == GatePhase::Open && !open.step_round && !open.origin && !open.paused_epoch);
    }
}

template<class Attempt, class Competitor>
void fail_during_competing_commit(PauseSource& source, std::atomic<std::size_t>& notifications,
                                 std::size_t allocation, Attempt attempt, Competitor competitor)
{
    Gate gate;
    std::optional<decltype(attempt())> result;
    bool injected = false;
    std::size_t attempts = 0;
    std::jthread caller([&] {
        exception_allocation_probe::Failure failure(allocation, &gate);
        result.emplace(attempt());
        injected = exception_allocation_probe::injected;
        attempts = exception_allocation_probe::attempts;
    });
    gate.entered.wait();
    // Allocation is outside the Pause mutex. This public competing operation
    // must commit while the first caller is held before throwing bad_alloc.
    competitor();
    const auto committed = source.snapshot();
    const auto committed_notifications = notifications.load();
    gate.release.count_down();
    caller.join();
    CHECK(injected && attempts == allocation + 1);
    CHECK(result && !result->has_value() && result->error() == PauseError::ResourceExhausted);
    same_snapshot(source.snapshot(), committed);
    CHECK(notifications == committed_notifications);
}

void root_oom_competitor(AdvanceMode mode)
{
    std::atomic<std::size_t> notifications{0};
    PauseSource source(mode, [&]() noexcept { ++notifications; });
    std::optional<BranchResult> winner;
    fail_during_competing_commit(source, notifications, 0,
        [&] { return source.make_root_branch(); },
        [&] { winner.emplace(source.make_root_branch()); CHECK(winner->has_value()); });
    const auto before = source.snapshot();
    CHECK(before.root_created && before.live_branch_count == 1 && before.revision == 1);
    // Resource preparation failed before lock validation. A retry without OOM
    // observes the winner and must now report the ordinary state error.
    auto retry = source.make_root_branch();
    CHECK(!retry && retry.error() == PauseError::RootAlreadyCreated);
    same_snapshot(source.snapshot(), before);
    winner.reset();
    CHECK(source.snapshot().live_branch_count == 0 && notifications == 2);
}

enum class IndependentRace { RootCommit, PeerCreate, Pause };
void independent_oom_competitor(IndependentRace race)
{
    std::atomic<std::size_t> notifications{0};
    PauseSource source(AdvanceMode::Continuous, [&]() noexcept { ++notifications; });
    std::optional<BranchResult> root, peer;
    if (race != IndependentRace::RootCommit) { root.emplace(source.make_root_branch()); CHECK(root->has_value()); }
    fail_during_competing_commit(source, notifications, 0,
        [&] { return source.make_branch(); }, [&] {
            if (race == IndependentRace::RootCommit) {
                root.emplace(source.make_root_branch()); CHECK(root->has_value());
            } else if (race == IndependentRace::PeerCreate) {
                peer.emplace(source.make_branch()); CHECK(peer->has_value());
            } else CHECK(source.request_pause());
        });
    if (race == IndependentRace::Pause) {
        const auto paused = source.snapshot();
        CHECK(paused.phase == GatePhase::Collecting && paused.live_branch_count == 1);
        auto rejected = source.make_branch();
        CHECK(!rejected && rejected.error() == PauseError::InvalidBranchState);
        same_snapshot(source.snapshot(), paused);
        root.reset();
        const auto empty = source.snapshot();
        CHECK(empty.phase == GatePhase::Paused && empty.live_branch_count == 0);
        CHECK(source.resume(*empty.paused_epoch, AdvanceMode::Continuous));
    }
    {
        const auto before = source.snapshot();
        auto retry = source.make_branch(); CHECK(retry);
        CHECK(source.snapshot().live_branch_count == before.live_branch_count + 1);
        CHECK(source.snapshot().revision == before.revision + 1);
    }
    peer.reset(); root.reset();
    const auto final = source.snapshot();
    CHECK(final.phase == GatePhase::Open && final.live_branch_count == 0 && final.waiting_branch_count == 0);
    CHECK(notifications == final.revision);
}

template<bool Dynamic>
auto fork_three(PauseBranch& parent)
{
    if constexpr (Dynamic) return parent.fork_and_block_self(3);
    else return parent.fork_and_block_self<3>();
}

template<bool Dynamic>
void fork_oom_competitor(bool collecting, std::optional<std::size_t> record)
{
    CHECK(Dynamic || record.has_value()); // No record selects dynamic reserve.
    std::atomic<std::size_t> notifications{0};
    PauseSource source(AdvanceMode::Continuous, [&]() noexcept { ++notifications; });
    auto root = source.make_root_branch(); CHECK(root);
    {
        auto parents = root->fork_and_block_self<2>(); CHECK(parents);
        std::optional<std::expected<std::array<PauseBranch, 2>, PauseError>> peer_children;
        // Dynamic fork allocates vector storage before the child records. The
        // default MSVC Debug vector also allocates its initial iterator proxy.
        // None of the noexcept proxy allocation points is faulted here: the
        // existing resource death tests separately verify those terminations.
        std::size_t allocation = record.value_or(0);
        if constexpr (Dynamic) {
            if (record) ++allocation;
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
            ++allocation;
#endif
        }
        fail_during_competing_commit(source, notifications, allocation,
            [&] { return fork_three<Dynamic>((*parents)[0]); }, [&] {
                if (collecting) CHECK(source.request_pause());
                peer_children.emplace((*parents)[1].fork_and_block_self<2>());
                CHECK(peer_children->has_value());
            });
        const auto before = source.snapshot();
        CHECK(before.phase == (collecting ? GatePhase::Collecting : GatePhase::Open));
        CHECK(before.live_branch_count == 3 && before.waiting_branch_count == 0);
        {
            auto retry = fork_three<Dynamic>((*parents)[0]); CHECK(retry && retry->size() == 3);
            const auto committed = source.snapshot();
            CHECK(committed.live_branch_count == 5 && committed.waiting_branch_count == 0);
            CHECK(committed.revision == before.revision + 1);
        }
        CHECK(source.snapshot().live_branch_count == 2);
        peer_children.reset();
        CHECK(source.snapshot().live_branch_count == 0);
        // Both delegated parents remain alive until their own children retire.
    }
    const auto final = source.snapshot();
    CHECK(final.live_branch_count == 0 && final.waiting_branch_count == 0);
    CHECK(final.phase == (collecting ? GatePhase::Paused : GatePhase::Open));
    CHECK(notifications == final.revision);
}

template<class Function>
void run_case(const char* id, const char* category, int count, Function function)
{
    current_id = id;
    for (current_iteration = 0; current_iteration != count; ++current_iteration) {
        const auto before = exception_allocation_probe::outstanding.load();
        function();
        CHECK(exception_allocation_probe::outstanding == before);
    }
    std::printf("PASS id=%s class=%s complete_scopes=%d iterations=%d\n", id, category, count, count);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    int count = 2, ordinary = 0;
    CHECK(argc % 2 == 1);
    for (int index = 1; index < argc; index += 2) {
        const std::string_view option(argv[index]), argument(argv[index + 1]);
        int parsed = 0;
        const auto [end, error] = std::from_chars(argument.data(), argument.data() + argument.size(), parsed);
        CHECK(error == std::errc{} && end == argument.data() + argument.size() && parsed > 0 && parsed <= 1000000);
        if (option == "--iterations") count = parsed;
        else { CHECK(option == "--ordinary-iterations"); ordinary = parsed; }
    }
    if (ordinary == 0) ordinary = count;
    for (bool nested : {false, true}) {
        for (auto gate : {GatePhase::Open, GatePhase::Collecting, GatePhase::Paused}) {
            const char* gate_name = gate == GatePhase::Open ? "open" : gate == GatePhase::Collecting ? "collecting" : "paused";
            char id[96];
            CHECK(std::snprintf(id, sizeof(id), "move.%s.%s", nested ? "nested" : "root", gate_name) > 0);
            run_case(id, "ordinary", ordinary, [=] { moved_delegated_parent(nested, gate); });
        }
    }
    run_case("api.resume-collecting-wrong-epoch", "ordinary", ordinary, resume_collecting_wrong_epoch);
    run_case("api.make-branch-paused", "ordinary", ordinary, make_branch_paused);
    run_case("api.root-recreate-after-retirement", "ordinary", ordinary, root_recreate_after_retirement);
    run_case("api.step-round-restarts", "ordinary", ordinary, step_round_restarts);
    run_case("oom.root.continuous", "concurrent", count, [] { root_oom_competitor(AdvanceMode::Continuous); });
    run_case("oom.root.single-step", "concurrent", count, [] { root_oom_competitor(AdvanceMode::SingleStep); });
    run_case("oom.independent.root-commit", "concurrent", count, [] { independent_oom_competitor(IndependentRace::RootCommit); });
    run_case("oom.independent.peer-create", "concurrent", count, [] { independent_oom_competitor(IndependentRace::PeerCreate); });
    run_case("oom.independent.pause", "concurrent", count, [] { independent_oom_competitor(IndependentRace::Pause); });
    run_case("oom.fork.dynamic.open.reserve", "concurrent", count,
             [] { fork_oom_competitor<true>(false, std::nullopt); });
    run_case("oom.fork.dynamic.collecting.reserve", "concurrent", count,
             [] { fork_oom_competitor<true>(true, std::nullopt); });
    for (bool dynamic : {false, true}) {
        for (bool collecting : {false, true}) {
            for (std::size_t record = 0; record != 3; ++record) {
                char id[96];
                CHECK(std::snprintf(id, sizeof(id), "oom.fork.%s.%s.record-%zu", dynamic ? "dynamic" : "fixed",
                                    collecting ? "collecting" : "open", record) > 0);
                run_case(id, "concurrent", count, [=] {
                    if (dynamic) fork_oom_competitor<true>(collecting, record);
                    else fork_oom_competitor<false>(collecting, record);
                });
            }
        }
    }
    std::printf("real Pause exception matrix: ordinary=%d concurrent=%d complete_scopes=%d state_families=29\n",
                ordinary * 10, count * 19, ordinary * 10 + count * 19);
}
