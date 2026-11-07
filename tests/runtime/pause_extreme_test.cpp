/* 测试项目：pause_extreme_test
 * 目的: 用真实 Pause 分支和 stdexec::task 检查嵌套 fork/join 与逐层退役.
 * 测试项:
 * - chain 深度 32/64/128, binary 深度 5, comb 深度 32.
 * - 每种形状/深度组合均覆盖连续运行, 单步运行, 屏障处取消, 外部业务 sender
 *   的 error 和 stopped, 共 25 个参数项; 外部非值完成需取消等待中的兄弟.
 * 关键预期: 根只完成一次且结果匹配路径; 子分支先于父分支退役; 正常路径完成
 * join 后父分支重新进入; 快照责任计数/epoch/revision 一致, 结束时责任清零.
 * 单步链的恢复次数应为 2 * depth + 1, 异常路径不得继续正常 join.
 * 入口: PAUSE_BUILD_TESTS=ON 时注册同名 CTest.
 *   ctest --test-dir <build> -C <config> -R "^pause_extreme_test$" --output-on-failure
 *   pause_extreme_test --suite all --iterations 10
 * 参数: suite 为 all/chain/binary/comb/structure/outcomes; structure 选择前三种
 * 路径, outcomes 选择外部 error/stopped; iterations 为 1..1000000, 默认 10.
 * 边界: 工作流与独立控制协程在同一 run_loop 上运行, 不覆盖 CPU 并发竞态.
 * 深度固定, 压力来自完整作用域的重复执行; 不探测无限递归或承诺任意深度栈安全.
 */
#include "../support/pause_test_notifications.hpp"
#include "pause.hpp"

#include <stdexec/execution.hpp>

#include <array>
#include <charconv>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Each repetition owns a complete public Pause scope. The independent control
// task yields through the approved run_loop rather than creating a thread per
// scope. This suite tests nesting and retirement; other suites test CPU races.
namespace {

const char* current_shape = "initialization";
const char* current_path = "initialization";
int current_depth = 0;
int current_iteration = 0;

[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr,
        "CHECK failed: shape=%s depth=%d path=%s iteration=%d line=%d: %s\n",
        current_shape, current_depth, current_path, current_iteration, line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

using coop_pause::AdvanceMode;
using coop_pause::GatePhase;
using coop_pause::PauseBranch;
using coop_pause::PauseSource;

enum class Shape { Chain, Binary, Comb };
enum class Path { Continuous, SingleStep, PausedCancel, ExternalError, ExternalStopped };
enum class Outcome { None, Value, Stopped, Error };

bool external_outcome(Path path) noexcept
{
    return path == Path::ExternalError || path == Path::ExternalStopped;
}

bool gated_leaves(Path path) noexcept
{
    return path == Path::PausedCancel || external_outcome(path);
}

const char* name(Shape value)
{
    switch (value) {
    case Shape::Chain: return "chain";
    case Shape::Binary: return "binary";
    case Shape::Comb: return "comb";
    }
    std::terminate();
}

const char* name(Path value)
{
    switch (value) {
    case Path::Continuous: return "continuous";
    case Path::SingleStep: return "single_step";
    case Path::PausedCancel: return "paused_cancel";
    case Path::ExternalError: return "external_error";
    case Path::ExternalStopped: return "external_stopped";
    }
    std::terminate();
}

struct Event {
    void* operation = nullptr;
    void (*complete)(void*, Outcome) noexcept = nullptr;

    void finish(Outcome outcome = Outcome::Value) noexcept
    {
        CHECK(operation && complete);
        // Completion can destroy the operation and the receiver synchronously.
        auto* local = operation;
        auto completion = complete;
        operation = nullptr;
        complete = nullptr;
        completion(local, outcome);
    }
};

struct EventSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(),
        stdexec::set_stopped_t(), stdexec::set_error_t(std::exception_ptr)>;
    Event* event;

    template<class Receiver>
    struct Operation {
        Event* event;
        Receiver receiver;

        void start() & noexcept
        {
            auto* local = event;
            CHECK(!local->operation && !local->complete);
            local->operation = this;
            local->complete = [](void* pointer, Outcome outcome) noexcept {
                auto* self = static_cast<Operation*>(pointer);
                if (outcome == Outcome::Value) stdexec::set_value(std::move(self->receiver));
                else if (outcome == Outcome::Stopped) stdexec::set_stopped(std::move(self->receiver));
                else stdexec::set_error(std::move(self->receiver),
                    std::make_exception_ptr(std::runtime_error("expected external business error")));
            };
        }
    };

    template<class Receiver>
    auto connect(Receiver receiver) const
    {
        return Operation<Receiver>{event, std::move(receiver)};
    }
};

struct Node {
    std::array<int, 2> children{-1, -1};
    int child_count = 0;
    bool entered = false;
    bool retired = false;
};

struct State {
    PauseSource* source;
    stdexec::run_loop* loop;
    stdexec::inplace_stop_source* stop;
    Shape shape;
    Path path;
    std::vector<Node> nodes;
    std::vector<Event> leaves;
    std::thread::id thread = std::this_thread::get_id();
    Outcome outcome = Outcome::None;
    int completions = 0;
    int assigned = 0;
    int entries = 0;
    int joins = 0;
    int retired = 0;
    int published_leaves = 0;
    std::size_t resumes = 0;
    std::size_t previous_revision = 0;
    std::size_t previous_epoch = 0;
    bool leaves_released = false;
    bool cancelled = false;
    bool external_completed = false;
};

void check_snapshot(State& state, const coop_pause::PauseSnapshot& snapshot)
{
    CHECK(snapshot.root_created);
    CHECK(snapshot.revision >= state.previous_revision);
    state.previous_revision = snapshot.revision;
    CHECK(snapshot.waiting_branch_count <= snapshot.live_branch_count);
    if (snapshot.phase == GatePhase::Open) {
        CHECK(!snapshot.origin && !snapshot.paused_epoch && !snapshot.step_round);
        CHECK(snapshot.waiting_branch_count == 0);
    } else {
        CHECK(snapshot.origin.has_value());
        if (snapshot.phase == GatePhase::Collecting) {
            CHECK(!snapshot.paused_epoch);
            CHECK(snapshot.waiting_branch_count < snapshot.live_branch_count);
        } else {
            CHECK(snapshot.paused_epoch.has_value());
            CHECK(snapshot.paused_epoch->value <= snapshot.revision);
            CHECK(snapshot.waiting_branch_count == snapshot.live_branch_count);
        }
        if (snapshot.origin == coop_pause::PauseOrigin::SingleStep)
            CHECK(snapshot.step_round.has_value());
    }
}

struct Witness {
    State* state;
    int id;

    Witness(State& value, int index) noexcept : state(&value), id(index) {}
    Witness(const Witness&) = delete;
    Witness(Witness&& other) noexcept
        : state(std::exchange(other.state, nullptr)), id(other.id) {}

    ~Witness()
    {
        if (!state) return;
        CHECK(std::this_thread::get_id() == state->thread);
        auto& node = state->nodes[id];
        CHECK(!node.retired);
        for (int index = 0; index != node.child_count; ++index)
            CHECK(state->nodes[node.children[index]].retired);
        node.retired = true;
        ++state->retired;
    }
};

struct OwnedBranch {
    // Reverse destruction retires the real Pause record before its witness.
    Witness witness;
    PauseBranch branch;

    OwnedBranch(State& state, PauseBranch value, int parent = -1)
        : witness(state, state.assigned++), branch(std::move(value))
    {
        CHECK(witness.id < static_cast<int>(state.nodes.size()));
        if (parent != -1) {
            auto& node = state.nodes[parent];
            CHECK(node.child_count < static_cast<int>(node.children.size()));
            node.children[node.child_count++] = witness.id;
        }
    }
    OwnedBranch(const OwnedBranch&) = delete;
    OwnedBranch(OwnedBranch&&) noexcept = default;
};

stdexec::task<void> tree(OwnedBranch owned, State& state, int depth)
{
    CHECK(std::this_thread::get_id() == state.thread);
    co_await owned.branch.suspend_point({static_cast<std::size_t>(owned.witness.id)});
    CHECK(std::this_thread::get_id() == state.thread);
    CHECK(!state.nodes[owned.witness.id].entered);
    state.nodes[owned.witness.id].entered = true;
    ++state.entries;
    if (depth == 0) {
        if (gated_leaves(state.path)) {
            const int index = state.published_leaves++;
            CHECK(index < static_cast<int>(state.leaves.size()));
            co_await EventSender{&state.leaves[index]};
            co_await owned.branch.suspend_point();
            CHECK(false); // Root/when_all cancellation must stop every waiting leaf.
        }
        co_return;
    }

    if (state.shape == Shape::Chain) {
        auto children = owned.branch.fork_and_block_self<1>();
        CHECK(children);
        co_await tree(OwnedBranch{state, std::move((*children)[0]), owned.witness.id},
                      state, depth - 1);
    } else {
        auto children = owned.branch.fork_and_block_self<2>();
        CHECK(children);
        co_await stdexec::when_all(
            tree(OwnedBranch{state, std::move((*children)[0]), owned.witness.id},
                 state, depth - 1),
            tree(OwnedBranch{state, std::move((*children)[1]), owned.witness.id},
                 state, state.shape == Shape::Comb ? 0 : depth - 1));
    }
    CHECK(!gated_leaves(state.path));
    const auto& node = state.nodes[owned.witness.id];
    for (int index = 0; index != node.child_count; ++index)
        CHECK(state.nodes[node.children[index]].retired);
    // Direct children must actually retire before the parent's atomic reentry.
    co_await owned.branch.suspend_point();
    CHECK(std::this_thread::get_id() == state.thread);
    ++state.joins;
}

stdexec::task<void> control(State& state)
{
    while (state.outcome == Outcome::None) {
        CHECK(std::this_thread::get_id() == state.thread);
        const auto snapshot = state.source->snapshot();
        check_snapshot(state, snapshot);
        if (state.path == Path::SingleStep && snapshot.phase == GatePhase::Paused
            && snapshot.waiting_branch_count != 0) {
            CHECK(snapshot.origin == coop_pause::PauseOrigin::SingleStep);
            CHECK(snapshot.step_round == state.resumes);
            CHECK(snapshot.paused_epoch->value > state.previous_epoch);
            state.previous_epoch = snapshot.paused_epoch->value;
            CHECK(state.source->resume(*snapshot.paused_epoch, AdvanceMode::SingleStep));
            ++state.resumes;
        } else if (gated_leaves(state.path) && !state.leaves_released
                   && state.published_leaves == static_cast<int>(state.leaves.size())) {
            CHECK(state.entries == static_cast<int>(state.nodes.size()));
            CHECK(snapshot.phase == GatePhase::Open);
            CHECK(snapshot.live_branch_count == state.leaves.size());
            CHECK(state.source->request_pause());
            state.leaves_released = true;
            // External-outcome cases retain one leaf on its business sender;
            // all siblings must be waiting in real Pause before it completes.
            const std::size_t first = external_outcome(state.path) ? 1 : 0;
            for (std::size_t index = first; index != state.leaves.size(); ++index)
                state.leaves[index].finish();
        } else if (state.path == Path::PausedCancel && state.leaves_released
                   && !state.cancelled && snapshot.phase == GatePhase::Paused) {
            CHECK(snapshot.waiting_branch_count == state.leaves.size());
            CHECK(snapshot.live_branch_count == state.leaves.size());
            state.cancelled = true;
            CHECK(state.stop->request_stop());
            const auto cancelled = state.source->snapshot();
            check_snapshot(state, cancelled);
            CHECK(cancelled.phase == GatePhase::Paused);
            CHECK(cancelled.live_branch_count == 0 && cancelled.waiting_branch_count == 0);
            CHECK(cancelled.paused_epoch == snapshot.paused_epoch);
        } else if (external_outcome(state.path) && state.leaves_released
                   && !state.external_completed && snapshot.phase == GatePhase::Collecting
                   && snapshot.waiting_branch_count + 1 == state.leaves.size()) {
            CHECK(snapshot.live_branch_count == state.leaves.size());
            CHECK(!state.stop->stop_requested());
            state.external_completed = true;
            state.leaves[0].finish(state.path == Path::ExternalError ? Outcome::Error : Outcome::Stopped);
        }
        // This is a separate queued controller, never a reentrant notifier.
        co_await stdexec::schedule(state.loop->get_scheduler());
    }
}

struct Receiver {
    using receiver_concept = stdexec::receiver_tag;
    State* state;
    stdexec::inplace_stop_token token;
    bool controller;

    auto get_env() const noexcept
    {
        return stdexec::env{
            stdexec::prop{stdexec::get_stop_token, token},
            stdexec::prop{stdexec::get_start_scheduler, state->loop->get_scheduler()}};
    }
    void set_value() noexcept
    {
        CHECK(std::this_thread::get_id() == state->thread);
        if (controller) {
            CHECK(state->outcome != Outcome::None);
            state->loop->finish();
        } else {
            CHECK(state->outcome == Outcome::None && state->completions++ == 0);
            state->outcome = Outcome::Value;
        }
    }
    void set_stopped() noexcept
    {
        CHECK(std::this_thread::get_id() == state->thread);
        CHECK(!controller && state->outcome == Outcome::None && state->completions++ == 0);
        state->outcome = Outcome::Stopped;
    }
    void set_error(std::exception_ptr error) noexcept
    {
        CHECK(std::this_thread::get_id() == state->thread);
        CHECK(!controller && state->outcome == Outcome::None && state->completions++ == 0);
        CHECK(state->path == Path::ExternalError);
        try { std::rethrow_exception(error); }
        catch (const std::runtime_error& expected) {
            CHECK(std::string_view(expected.what()) == "expected external business error");
        }
        catch (...) { CHECK(false); }
        state->outcome = Outcome::Error;
    }
};

struct Counts {
    std::uint64_t scopes = 0;
    std::uint64_t entries = 0;
    std::uint64_t joins = 0;
    std::uint64_t retired = 0;
    std::uint64_t resumes = 0;
    std::uint64_t values = 0;
    std::uint64_t stopped = 0;
    std::uint64_t errors = 0;
};

void run_scope(Shape shape, int depth, Path path, Counts& counts)
{
    const int nodes = shape == Shape::Chain ? depth + 1
        : shape == Shape::Comb ? 2 * depth + 1 : (1 << (depth + 1)) - 1;
    const int leaves = shape == Shape::Chain ? 1
        : shape == Shape::Comb ? depth + 1 : 1 << depth;
    stdexec::run_loop loop;
    stdexec::inplace_stop_source stop, control_stop;
    std::size_t notifications = 0;
    PauseSource source(path == Path::SingleStep ? AdvanceMode::SingleStep : AdvanceMode::Continuous, pause_test::notifications([&]() noexcept { ++notifications; }));
    auto root = source.make_root_branch();
    CHECK(root);
    State state{&source, &loop, &stop, shape, path,
                std::vector<Node>(nodes), std::vector<Event>(leaves)};
    {
        auto operation = stdexec::connect(stdexec::starts_on(loop.get_scheduler(),
            tree(OwnedBranch{state, std::move(*root)}, state, depth)),
            Receiver{&state, stop.get_token(), false});
        auto controller = stdexec::connect(stdexec::starts_on(loop.get_scheduler(), control(state)),
            Receiver{&state, control_stop.get_token(), true});
        stdexec::start(operation);
        stdexec::start(controller);
        loop.run();
        const auto expected = path == Path::ExternalError ? Outcome::Error
            : gated_leaves(path) ? Outcome::Stopped : Outcome::Value;
        CHECK(state.outcome == expected);
        CHECK(state.completions == 1);
        // Operation destruction releases stopped frames after every CPO,
        // request_stop, callback, notification, and scheduler tail has returned.
    }
    CHECK(state.assigned == nodes && state.entries == nodes && state.retired == nodes);
    CHECK(state.joins == (gated_leaves(path) ? 0 : nodes - leaves));
    CHECK(state.cancelled == (path == Path::PausedCancel));
    CHECK(state.external_completed == external_outcome(path));
    CHECK(stop.stop_requested() == (path == Path::PausedCancel));
    if (path == Path::SingleStep) {
        CHECK(state.resumes != 0 && state.resumes <= static_cast<std::size_t>(2 * nodes - leaves));
        if (shape == Shape::Chain) CHECK(state.resumes == static_cast<std::size_t>(2 * depth + 1));
    } else CHECK(state.resumes == 0);
    const auto final = source.snapshot();
    check_snapshot(state, final);
    CHECK(final.live_branch_count == 0 && final.waiting_branch_count == 0);
    CHECK(final.phase == (path == Path::Continuous ? GatePhase::Open : GatePhase::Paused));
    CHECK(path == Path::Continuous ? notifications == 0 : notifications != 0);
    for (const auto& node : state.nodes) CHECK(node.entered && node.retired);
    for (const auto& leaf : state.leaves) CHECK(!leaf.operation && !leaf.complete);
    ++counts.scopes;
    counts.entries += state.entries;
    counts.joins += state.joins;
    counts.retired += state.retired;
    counts.resumes += state.resumes;
    counts.values += state.outcome == Outcome::Value;
    counts.stopped += state.outcome == Outcome::Stopped;
    counts.errors += state.outcome == Outcome::Error;
}

void run_case(Shape shape, int depth, Path path, int iterations)
{
    current_shape = name(shape);
    current_path = name(path);
    current_depth = depth;
    Counts counts;
    for (int iteration = 0; iteration != iterations; ++iteration) {
        current_iteration = iteration;
        run_scope(shape, depth, path, counts);
    }
    CHECK(counts.scopes == static_cast<std::uint64_t>(iterations));
    std::printf("PASS shape=%s depth=%d path=%s scopes=%" PRIu64
                " entries=%" PRIu64 " joins=%" PRIu64 " retired=%" PRIu64
                " resumes=%" PRIu64 " value=%" PRIu64 " stopped=%" PRIu64 " error=%" PRIu64 "\n",
                name(shape), depth, name(path), counts.scopes, counts.entries, counts.joins,
                counts.retired, counts.resumes, counts.values, counts.stopped, counts.errors);
    std::fflush(stdout);
}

} // namespace

int main(int argc, char** argv)
{
    int iterations = 10;
    std::string_view suite = "all";
    CHECK(argc % 2 == 1);
    for (int index = 1; index < argc; index += 2) {
        const std::string_view option = argv[index];
        const std::string_view value = argv[index + 1];
        if (option == "--iterations") {
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), iterations);
            CHECK(error == std::errc{} && end == value.data() + value.size());
            CHECK(iterations > 0 && iterations <= 1000000);
        } else {
            CHECK(option == "--suite");
            CHECK(value == "all" || value == "chain" || value == "binary" || value == "comb"
                  || value == "structure" || value == "outcomes");
            suite = value;
        }
    }
    for (const auto shape : {Shape::Chain, Shape::Binary, Shape::Comb}) {
        if (suite != "all" && suite != "structure" && suite != "outcomes" && suite != name(shape)) continue;
        for (const int depth : {32, 64, 128}) {
            if (shape != Shape::Chain && depth != 32) continue;
            const int actual_depth = shape == Shape::Binary ? 5 : depth;
            for (const auto path : {Path::Continuous, Path::SingleStep, Path::PausedCancel,
                                    Path::ExternalError, Path::ExternalStopped}) {
                if (suite == "outcomes" && !external_outcome(path)) continue;
                if (suite == "structure" && external_outcome(path)) continue;
                run_case(shape, actual_depth, path, iterations);
            }
        }
    }
}
