/* 测试项目：pause_batch_extreme_test
 * 目的: 使用真实 Pause 和 run_loop 检查批量恢复, 排队后取消及完成回调重入.
 * 测试项:
 * - matrix: 单工作线程, 宽度 1/2/8/64/256, Continuous/SingleStep,
 *   分别组合不取消/全部取消/交替取消/首尾取消, 共 40 个参数项.
 * - parallel: 4 个工作线程, 宽度 64, 上述模式和取消组合, 共 8 个参数项.
 * - reentrant: 宽度 2/64 和两种恢复模式; 首个接收者销毁自身 operation,
 *   取消兄弟并再次进入暂停点, 其余批节点此时尚未全部交给调度器, 共 4 项.
 * 关键预期: 每批节点只调度/完成一次, 回到所属线程; 排队取消不会提前完成或
 * 改写 revision; 单步幸存者进入下一屏障, 全取消也可恢复空批次; 最终责任清零.
 * 入口: PAUSE_BUILD_TESTS=ON 时注册同名 CTest.
 *   ctest --test-dir <build> -C <config> -R "^pause_batch_extreme_test$" --output-on-failure
 *   pause_batch_extreme_test --suite all --iterations 10
 * 参数: suite 为 all/matrix/parallel/reentrant; iterations 为 1..1000000,
 * 默认 all 和每项 10 次. 增大重复次数不会增大递归深度.
 * 边界: 调度器外壳仅计数和控制宿主时序, 实际排队/取消仍由 run_loop 执行;
 * 不替代任意第三方调度器兼容性或全部操作系统线程交错的验证.
 */
#include "pause.hpp"

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <vector>

namespace {
[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

using coop_pause::AdvanceMode;
using coop_pause::GatePhase;
enum class Cancel { None, All, Alternating, Ends, AfterFirst };
enum class Result { None, Value, Stopped };

struct Worker {
    stdexec::run_loop loop;
    std::latch ready{1};
    std::thread::id id;
    std::jthread thread;
    Worker() : thread([this] {
        id = std::this_thread::get_id();
        ready.count_down();
        loop.run();
    }) { ready.wait(); }
    ~Worker() { loop.finish(); thread.join(); }
};

struct Pool {
    std::vector<std::unique_ptr<Worker>> workers;
    explicit Pool(std::size_t count)
    {
        for (std::size_t index = 0; index != count; ++index)
            workers.push_back(std::make_unique<Worker>());
    }
};

struct Round;
struct Node {
    Round* round = nullptr;
    Worker* worker = nullptr;
    std::size_t index = 0;
    std::optional<coop_pause::PauseBranch> branch;
    stdexec::inplace_stop_source stop;
    std::atomic<Result> result[2]{Result::None, Result::None};
    std::atomic<int> schedules[2]{0, 0};
    std::atomic<int> starts[2]{0, 0};
    void* operation[2]{nullptr, nullptr};
    void (*destroy)(void*) noexcept = nullptr;
};

void before_schedule(Node* node, int stage) noexcept;
struct ScheduleSender;
struct Scheduler {
    using scheduler_concept = stdexec::scheduler_tag;
    Node* node;
    int stage;
    bool operator==(const Scheduler&) const noexcept = default;
    auto query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
    { return *this; }
    auto schedule() const noexcept -> ScheduleSender;
};

// Only a transparent counting facade: queueing and cancellation execute in
// the pinned, supported run_loop backend, with the receiver's original token.
struct ScheduleSender {
    using sender_concept = stdexec::sender_tag;
    using Base = stdexec::schedule_result_t<stdexec::run_loop::scheduler>;
    Node* node;
    int stage;
    struct Attrs {
        Node* node;
        int stage;
        template<class Tag>
            requires (std::same_as<Tag, stdexec::set_value_t>
                      || std::same_as<Tag, stdexec::set_stopped_t>)
        auto query(stdexec::get_completion_scheduler_t<Tag>) const noexcept
        { return Scheduler{node, stage}; }
    };
    auto get_env() const noexcept { return Attrs{node, stage}; }
    template<class, class Env>
    static consteval auto get_completion_signatures() noexcept
    { return stdexec::completion_signatures_of_t<Base, Env>{}; }
    template<class R>
    struct ForwardReceiver {
        using receiver_concept = stdexec::receiver_tag;
        R receiver;
        decltype(auto) get_env() const noexcept { return stdexec::get_env(receiver); }
        void set_value() noexcept { stdexec::set_value(std::move(receiver)); }
        void set_stopped() noexcept { stdexec::set_stopped(std::move(receiver)); }
    };
    template<class R>
    struct Operation {
        Node* node;
        int stage;
        stdexec::connect_result_t<Base, ForwardReceiver<R>> inner;
        Operation(Node* current, int pass, R receiver) noexcept
            : node(current), stage(pass), inner(stdexec::connect(
                  stdexec::schedule(current->worker->loop.get_scheduler()),
                  ForwardReceiver<R>{std::move(receiver)})) {}
        void start() & noexcept { ++node->starts[stage]; stdexec::start(inner); }
    };
    template<class R>
    auto connect(R receiver) const noexcept
    {
        CHECK(stdexec::get_stop_token(stdexec::get_env(receiver)) == node->stop.get_token());
        return Operation<R>{node, stage, std::move(receiver)};
    }
};
auto Scheduler::schedule() const noexcept -> ScheduleSender
{
    ++node->schedules[stage];
    before_schedule(node, stage);
    return {node, stage};
}
static_assert(stdexec::scheduler<Scheduler>);

struct Receiver {
    using receiver_concept = stdexec::receiver_tag;
    Node* node;
    int stage;
    auto get_env() const noexcept
    {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, node->stop.get_token()},
                            stdexec::prop{stdexec::get_start_scheduler, Scheduler{node, stage}}};
    }
    void complete(Result result) noexcept;
    void set_value() noexcept { complete(Result::Value); }
    void set_stopped() noexcept { complete(Result::Stopped); }
    template<class Error> void set_error(Error&&) noexcept { CHECK(false); }
};
using Sender = decltype(std::declval<coop_pause::PauseBranch&>().suspend_point());
using Operation = stdexec::connect_result_t<Sender, Receiver>;

void create_operation(Node& node, int stage)
{
    CHECK(node.operation[stage] == nullptr);
    node.operation[stage] = new Operation(stdexec::connect(
        node.branch->suspend_point(coop_pause::PausePointInfo{node.index}), Receiver{&node, stage}));
    node.destroy = [](void* pointer) noexcept { delete static_cast<Operation*>(pointer); };
}
void start_operation(Node& node, int stage) noexcept
{
    stdexec::start(*static_cast<Operation*>(node.operation[stage]));
}

struct EntryReceiver {
    using receiver_concept = stdexec::receiver_tag;
    Round* round;
    std::size_t worker;
    void set_value() noexcept;
    void set_stopped() noexcept { CHECK(false); }
    template<class Error> void set_error(Error&&) noexcept { CHECK(false); }
};
using EntryOperation = stdexec::connect_result_t<
    stdexec::schedule_result_t<stdexec::run_loop::scheduler>, EntryReceiver>;

bool cancelled(Cancel pattern, std::size_t index, std::size_t width) noexcept
{
    switch (pattern) {
    case Cancel::None: return false;
    case Cancel::All: return true;
    case Cancel::Alternating: return index % 2 == 0;
    case Cancel::Ends: return index == 0 || index + 1 == width;
    case Cancel::AfterFirst: return index != 0;
    }
    std::terminate();
}
std::size_t survivors(Cancel pattern, std::size_t width) noexcept
{
    std::size_t count = 0;
    for (std::size_t index = 0; index != width; ++index)
        count += !cancelled(pattern, index, width);
    return count;
}

struct Round {
    Pool& pool;
    const std::size_t width;
    const AdvanceMode mode;
    const Cancel pattern;
    const std::size_t live;
    coop_pause::PauseSource source{AdvanceMode::Continuous, []() noexcept {}};
    std::optional<coop_pause::PauseBranch> root;
    std::vector<Node> nodes;
    std::vector<std::unique_ptr<EntryOperation>> entries;
    std::latch started;
    std::latch release{1};
    std::latch entries_finished;
    std::latch primary_finished;
    std::latch secondary_finished;
    std::latch first_finished{1};

    Round(Pool& workers, std::size_t size, AdvanceMode advance, Cancel cancel)
        : pool(workers), width(size), mode(advance), pattern(cancel),
          live(survivors(cancel, size)), nodes(size),
          started(static_cast<std::ptrdiff_t>(workers.workers.size())),
          entries_finished(static_cast<std::ptrdiff_t>(workers.workers.size())),
          primary_finished(static_cast<std::ptrdiff_t>(size)),
          secondary_finished(static_cast<std::ptrdiff_t>(live))
    {
        auto branch = source.make_root_branch(); CHECK(branch);
        root.emplace(std::move(*branch));
        auto children = root->fork_and_block_self(width); CHECK(children);
        for (std::size_t index = 0; index != width; ++index) {
            auto& node = nodes[index];
            node.round = this;
            node.worker = pool.workers[index % pool.workers.size()].get();
            node.index = index;
            node.branch.emplace(std::move((*children)[index]));
            create_operation(node, 0);
        }
        CHECK(source.request_pause());
        for (std::size_t index = 0; index != pool.workers.size(); ++index) {
            entries.emplace_back(new EntryOperation(stdexec::connect(
                stdexec::schedule(pool.workers[index]->loop.get_scheduler()),
                EntryReceiver{this, index})));
        }
    }

    void stop_selected() noexcept
    {
        for (std::size_t index = 0; index != width; ++index) {
            if (cancelled(pattern, index, width)) {
                CHECK(!nodes[index].stop.request_stop());
                CHECK(nodes[index].stop.request_stop());
            }
        }
    }

    void run()
    {
        for (const auto& entry : entries) stdexec::start(*entry);
        started.wait();
        const auto paused = source.snapshot();
        CHECK(paused.phase == GatePhase::Paused && paused.waiting_branch_count == width);
        CHECK(paused.live_branch_count == width && paused.paused_epoch);
        if (pattern == Cancel::AfterFirst) release.count_down();
        CHECK(source.resume(*paused.paused_epoch, mode));
        if (pattern != Cancel::AfterFirst) {
            const auto queued = source.snapshot();
            CHECK(queued.waiting_branch_count == 0 && queued.live_branch_count == width);
            CHECK(queued.phase == (mode == AdvanceMode::SingleStep
                                      ? GatePhase::Collecting : GatePhase::Open));
            stop_selected();
            // Queued cancellation is not early completion or withdrawal.
            CHECK(source.snapshot().revision == queued.revision);
            for (const auto& node : nodes) CHECK(node.result[0] == Result::None);
            release.count_down();
        }
        primary_finished.wait();
        entries_finished.wait();
        const auto after = source.snapshot();
        CHECK(after.live_branch_count == live);
        if (mode == AdvanceMode::SingleStep) {
            CHECK(after.phase == GatePhase::Paused && after.paused_epoch);
            CHECK(after.waiting_branch_count == live);
            CHECK(after.paused_epoch != paused.paused_epoch);
            // Also exercises the empty batch when every branch stopped.
            CHECK(source.resume(*after.paused_epoch, AdvanceMode::Continuous));
        } else {
            CHECK(after.phase == GatePhase::Open && after.waiting_branch_count == 0);
        }
        secondary_finished.wait();
        // Seeing a latch reach zero does not join the notifying member
        // function's return tail. Queue one fence behind every receiver so
        // none can still touch this Round (including its latches) at teardown.
        for (const auto& worker : pool.workers)
            CHECK(stdexec::sync_wait(stdexec::schedule(worker->loop.get_scheduler())).has_value());
        for (const auto& node : nodes) {
            const bool stopped = cancelled(pattern, node.index, width);
            CHECK(node.result[0] == (stopped ? Result::Stopped : Result::Value));
            CHECK(node.result[1] == (stopped ? Result::None : Result::Value));
            CHECK(node.operation[0] == nullptr && node.operation[1] == nullptr);
            CHECK(node.schedules[0] == 1 && node.starts[0] == 1);
            const int second = !stopped && mode == AdvanceMode::SingleStep ? 1 : 0;
            CHECK(node.schedules[1] == second && node.starts[1] == second);
        }
        CHECK(source.snapshot().phase == GatePhase::Open);
        CHECK(source.snapshot().waiting_branch_count == 0);
        CHECK(source.snapshot().live_branch_count == live);
        // Cancellation withdrew responsibility while every child record was
        // retained. Only now release the records, before their delegated root.
        for (auto& node : nodes) node.branch.reset();
        CHECK(source.snapshot().live_branch_count == 0);
        root.reset();
    }
};

void EntryReceiver::set_value() noexcept
{
    auto* local = round;
    for (std::size_t index = worker; index < local->width; index += local->pool.workers.size())
        start_operation(local->nodes[index], 0);
    local->started.count_down();
    local->release.wait();
    local->entries_finished.count_down();
}

void before_schedule(Node* node, int stage) noexcept
{
    if (stage == 0 && node->round->pattern == Cancel::AfterFirst && node->index == 1) {
        // The first public receiver must have destroyed its operation,
        // cancelled siblings and reentered before the second batch node is
        // handed to its scheduler. Remaining private batch nodes stay alive.
        node->round->first_finished.wait();
        CHECK(node->round->nodes[0].operation[0] == nullptr);
    }
}

void Receiver::complete(Result result) noexcept
{
    auto* local = node;
    const int pass = stage;
    auto* round = local->round;
    CHECK(std::this_thread::get_id() == local->worker->id);
    CHECK(local->result[pass].exchange(result) == Result::None);
    auto* operation = std::exchange(local->operation[pass], nullptr);
    CHECK(operation != nullptr);
    local->destroy(operation);
    // Everything below uses saved external state: this receiver was destroyed
    // synchronously by its own terminal completion.
    if (pass == 1) {
        CHECK(result == Result::Value);
        round->secondary_finished.count_down();
        return;
    }
    if (round->pattern == Cancel::AfterFirst && local->index == 0) {
        CHECK(result == Result::Value);
        round->stop_selected();
    }
    if (result == Result::Value) {
        create_operation(*local, 1);
        start_operation(*local, 1);
    }
    if (round->pattern == Cancel::AfterFirst && local->index == 0)
        round->first_finished.count_down();
    round->primary_finished.count_down();
}

const char* name(Cancel pattern) noexcept
{
    switch (pattern) {
    case Cancel::None: return "none";
    case Cancel::All: return "all";
    case Cancel::Alternating: return "alternating";
    case Cancel::Ends: return "ends";
    case Cancel::AfterFirst: return "completion-cancels-siblings";
    }
    std::terminate();
}

void run_case(Pool& pool, const char* suite, std::size_t width, AdvanceMode mode,
              Cancel cancel, int count)
{
    for (int iteration = 0; iteration != count; ++iteration)
        Round(pool, width, mode, cancel).run();
    std::printf("PASS: suite=%s width=%zu workers=%zu mode=%s cancel=%s iterations=%d\n",
                suite, width, pool.workers.size(),
                mode == AdvanceMode::SingleStep ? "single-step" : "continuous", name(cancel), count);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    int count = 10;
    std::string_view suite = "all";
    for (int index = 1; index < argc; index += 2) {
        CHECK(index + 1 < argc);
        const std::string_view option(argv[index]), value(argv[index + 1]);
        if (option == "--suite") suite = value;
        else {
            CHECK(option == "--iterations");
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
            CHECK(error == std::errc{} && end == value.data() + value.size());
            CHECK(count > 0 && count <= 1000000);
        }
    }
    CHECK(suite == "all" || suite == "matrix" || suite == "parallel" || suite == "reentrant");
    if (suite == "all" || suite == "matrix") {
        Pool pool(1);
        for (std::size_t width : {1u, 2u, 8u, 64u, 256u})
            for (auto mode : {AdvanceMode::Continuous, AdvanceMode::SingleStep})
                for (auto cancel : {Cancel::None, Cancel::All, Cancel::Alternating, Cancel::Ends})
                    run_case(pool, "matrix", width, mode, cancel, count);
    }
    if (suite == "all" || suite == "parallel") {
        Pool pool(4);
        for (auto mode : {AdvanceMode::Continuous, AdvanceMode::SingleStep})
            for (auto cancel : {Cancel::None, Cancel::All, Cancel::Alternating, Cancel::Ends})
                run_case(pool, "parallel", 64, mode, cancel, count);
    }
    if (suite == "all" || suite == "reentrant") {
        Pool pool(1);
        for (std::size_t width : {2u, 64u})
            for (auto mode : {AdvanceMode::Continuous, AdvanceMode::SingleStep})
                run_case(pool, "reentrant", width, mode, Cancel::AfterFirst, count);
    }
}
