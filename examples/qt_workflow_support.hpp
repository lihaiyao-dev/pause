#ifndef PAUSE_EXAMPLES_QT_WORKFLOW_SUPPORT_HPP_
#define PAUSE_EXAMPLES_QT_WORKFLOW_SUPPORT_HPP_

// Keep this declaration before the sender includes. It bounds clice's header
// preamble: g1b5d0f3 crashes when reloading these stdexec types from a PCH.
namespace pause_graph {
enum class Operation { Step, Sequence, Parallel, Race, Choice, Optional, RecoverStop, OnSecond };
}

#include "example_support.hpp"
#include <exec/timed_thread_scheduler.hpp>
#include <algorithm>
#include <chrono>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pause_graph {
// Explicit display metadata; it does not execute business operations.
struct Options {
    int min_ms = 400;
    int max_ms = 8000;
    bool pre_stopped = false;
};

struct Flow {
    Operation operation;
    std::string key;
    std::string title;
    std::vector<Flow> children;

    bool selected = true;
    int entry = -1;
    int exit = -1;
};

inline Flow step(std::string key, std::string title)
{
    return {Operation::Step, std::move(key), std::move(title), {}};
}
inline Flow sequence(std::initializer_list<Flow> children)
{
    return {Operation::Sequence, {}, {}, children};
}
inline Flow parallel(std::string key, std::string title, std::vector<Flow> children)
{
    return {Operation::Parallel, std::move(key), std::move(title), std::move(children)};
}
inline Flow race(std::string key, std::string title, std::initializer_list<Flow> children)
{
    return {Operation::Race, std::move(key), std::move(title), children};
}
inline Flow optional(std::string key, std::string title, bool enabled, Flow child)
{
    return {Operation::Optional, std::move(key), std::move(title), {std::move(child)}, enabled};
}
inline Flow choose(std::string key, std::string title, bool alternate, Flow primary, Flow fallback)
{
    return {Operation::Choice, std::move(key), std::move(title),
        {std::move(primary), std::move(fallback)}, alternate};
}
inline Flow recover_stopped(std::string key, std::string title, Flow child)
{
    return {Operation::RecoverStop, std::move(key), std::move(title), {std::move(child)}};
}
inline Flow on_second(std::string key, std::string title, Flow child)
{
    return {Operation::OnSecond, std::move(key), std::move(title), {std::move(child)}};
}
// Automatic graph layout.
enum class NodeKind { Work, Fork, Join, Decision, Controller };
struct NodeSpec {
    std::string key;
    std::string title;
    NodeKind kind;
    int column;
    double row;
};
struct Edge { int from; int to; std::string label; bool enabled = true; };
struct Graph {
    Flow root;
    std::vector<NodeSpec> nodes;
    std::vector<Edge> edges;
    int columns = 0;
    double rows = 0;
    int controller = -1;

    int find(const std::string& key) const
    {
        for (int i = 0; i < static_cast<int>(nodes.size()); ++i)
            if (nodes[i].key == key) return i;
        throw std::invalid_argument("Unknown graph node: " + key);
    }
};

namespace graph_detail {
struct Size { int width; int height; };
inline Size measure(const Flow& flow)
{
    if (flow.operation == Operation::Step) return {1, 1};
    if (flow.operation == Operation::Sequence) {
        if (flow.children.empty()) throw std::invalid_argument("sequence requires at least one child");
        Size result{0, 1};
        for (const auto& child : flow.children) {
            const auto size = measure(child);
            result.width += size.width;
            result.height = std::max(result.height, size.height);
        }
        return result;
    }
    Size result{2, 0};
    int width = 0;
    for (const auto& child : flow.children) {
        const auto size = measure(child);
        width = std::max(width, size.width);
        result.height += size.height;
    }
    result.width += width;
    result.height = std::max(1, result.height);
    return result;
}

inline int node(Graph& graph, std::string key, std::string title, NodeKind kind,
                int column, double row)
{
    const int id = static_cast<int>(graph.nodes.size());
    graph.nodes.push_back({std::move(key), std::move(title), kind, column, row});
    return id;
}

inline void compile(Graph& graph, Flow& flow, int column, double row)
{
    const auto size = measure(flow);
    const auto center = row + (size.height - 1) / 2.0;
    if (flow.operation == Operation::Step) {
        flow.entry = flow.exit = node(graph, flow.key, flow.title, NodeKind::Work, column, center);
        return;
    }
    if (flow.operation == Operation::Sequence) {
        int previous = -1;
        for (auto& child : flow.children) {
            const auto child_size = measure(child);
            compile(graph, child, column, row + (size.height - child_size.height) / 2.0);
            if (previous >= 0) graph.edges.push_back({previous, child.entry, {}});
            else flow.entry = child.entry;
            previous = flow.exit = child.exit;
            column += child_size.width;
        }
        return;
    }
    const bool decision = flow.operation == Operation::Choice || flow.operation == Operation::Optional;
    flow.entry = node(graph, flow.key + ".fork", flow.title,
        decision ? NodeKind::Decision : NodeKind::Fork, column, center);
    std::vector<std::pair<int, bool>> exits;
    for (int i = 0; i < static_cast<int>(flow.children.size()); ++i) {
        auto& child = flow.children[i];
        compile(graph, child, column + 1, row);
        std::string label;
        if (flow.operation == Operation::Choice) label = i == 0 ? "常规" : "备用";
        if (flow.operation == Operation::Optional) label = "执行";
        const bool enabled = flow.operation == Operation::Choice ? flow.selected == (i == 1)
            : flow.operation == Operation::Optional ? flow.selected : true;
        graph.edges.push_back({flow.entry, child.entry, std::move(label), enabled});
        exits.push_back({child.exit, enabled});
        row += measure(child).height;
    }
    flow.exit = node(graph, flow.key + ".join", flow.title + "：汇合", NodeKind::Join,
        column + size.width - 1, center);
    for (const auto [exit, enabled] : exits) graph.edges.push_back({exit, flow.exit, {}, enabled});
    if (exits.empty() || flow.operation == Operation::Optional)
        graph.edges.push_back({flow.entry, flow.exit, exits.empty() ? "0 个任务" : "跳过", exits.empty() || !flow.selected});
}
} // namespace graph_detail

inline Graph make_graph(Flow layout)
{
    Graph graph;
    graph.root = std::move(layout);
    const auto size = graph_detail::measure(graph.root);
    graph.columns = size.width;
    graph.rows = size.height + 1.5;
    graph_detail::compile(graph, graph.root, 0, 0);
    graph.controller = graph_detail::node(graph, "controller", "会话控制（不参与屏障）",
        NodeKind::Controller, 0, size.height + 0.5);
    std::unordered_set<std::string> keys;
    for (const auto& node : graph.nodes)
        if (!keys.insert(node.key).second) throw std::invalid_argument("Duplicate graph key: " + node.key);
    for (const auto& edge : graph.edges)
        if (graph.nodes[edge.from].column >= graph.nodes[edge.to].column)
            throw std::invalid_argument("Graph edges must point to a later column");
    return graph;
}

// Thread-safe display snapshots and timings.
using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
enum class NodeStatus { Pending, Waiting, Running, Joining, Completed, Skipped, Cancelled, Stopped, Failed, Abandoned };
enum class Completion { Active, Value, Stopped, Error };
struct NodeProgress {
    NodeStatus status = NodeStatus::Pending;
    int duration_ms = 0;
    Clock::time_point started{};
    Milliseconds elapsed{0};
    std::thread::id thread;
    std::string note;
};
struct Progress {
    std::vector<NodeProgress> nodes;
    Completion completion = Completion::Active;
    std::string error;
};

// Display data only: this store never opens/closes the PauseSource gate.
class ProgressStore {
    Graph graph_;
    mutable std::mutex mutex_;
    Progress progress_;
    std::mt19937 generator_{std::random_device{}()};
    std::uniform_int_distribution<int> duration_;

    static const Flow* lookup(const Flow& flow, const std::string& key)
    {
        if (flow.key == key) return &flow;
        for (const auto& child : flow.children)
            if (const auto* found = lookup(child, key)) return found;
        return nullptr;
    }
    void retire_unlocked(const Flow& flow) noexcept
    {
        for (int i = flow.entry; i <= flow.exit; ++i) {
            auto& node = progress_.nodes[i];
            if (node.status == NodeStatus::Running || node.status == NodeStatus::Joining)
                node.elapsed = std::chrono::duration_cast<Milliseconds>(Clock::now() - node.started);
            if (node.status == NodeStatus::Waiting || node.status == NodeStatus::Running || node.status == NodeStatus::Joining)
                node.status = NodeStatus::Cancelled;
            else if (node.status == NodeStatus::Pending) node.status = NodeStatus::Abandoned;
        }
    }
public:
    ProgressStore(Flow layout, const Options& options)
        : graph_(make_graph(std::move(layout)))
    {
        if (options.min_ms <= 0 || options.max_ms < options.min_ms)
            throw std::invalid_argument("invalid duration range");
        duration_ = std::uniform_int_distribution<int>(options.min_ms, options.max_ms);
        progress_.nodes.resize(graph_.nodes.size());
        set(graph_.controller, NodeStatus::Running);
    }
    const Graph& graph() const noexcept { return graph_; }
    const Flow& scope(const std::string& key) const
    {
        const auto* found = lookup(graph_.root, key);
        if (!found) throw std::invalid_argument("Unknown display scope: " + key);
        return *found;
    }
    int random_duration()
    {
        std::lock_guard lock(mutex_); return duration_(generator_);
    }
    void plan(int id, int milliseconds)
    {
        if (milliseconds < 0) throw std::invalid_argument("negative planned duration");
        std::lock_guard lock(mutex_); progress_.nodes[id].duration_ms = milliseconds;
    }
    void set(int id, NodeStatus next, std::string note = {})
    {
        std::lock_guard lock(mutex_);
        auto& node = progress_.nodes[id];
        const auto now = Clock::now();
        if (node.status == NodeStatus::Running || node.status == NodeStatus::Joining)
            node.elapsed = std::chrono::duration_cast<Milliseconds>(now - node.started);
        if (next == NodeStatus::Running || next == NodeStatus::Joining) {
            node.started = now; node.thread = std::this_thread::get_id();
        }
        node.status = next;
        if (!note.empty()) node.note = std::move(note);
    }
    void annotate(int id, std::string note)
    {
        std::lock_guard lock(mutex_); progress_.nodes[id].note = std::move(note);
    }
    std::string note(int id) const
    {
        std::lock_guard lock(mutex_); return progress_.nodes[id].note;
    }
    void skip(const Flow& scope)
    {
        std::lock_guard lock(mutex_);
        for (int i = scope.entry; i <= scope.exit; ++i) progress_.nodes[i].status = NodeStatus::Skipped;
    }
    void retire(const Flow& scope) noexcept
    {
        std::lock_guard lock(mutex_); retire_unlocked(scope);
    }
    void finish(Completion completion, std::string error)
    {
        std::lock_guard lock(mutex_);
        retire_unlocked(graph_.root);
        auto& control = progress_.nodes[graph_.controller];
        control.elapsed = std::chrono::duration_cast<Milliseconds>(Clock::now() - control.started);
        control.status = completion == Completion::Value ? NodeStatus::Completed : NodeStatus::Cancelled;
        progress_.completion = completion; progress_.error = std::move(error);
    }
    Progress snapshot() const
    {
        std::lock_guard lock(mutex_);
        auto result = progress_;
        for (auto& node : result.nodes)
            if (node.status == NodeStatus::Running || node.status == NodeStatus::Joining)
                node.elapsed = std::chrono::duration_cast<Milliseconds>(Clock::now() - node.started);
        return result;
    }
};

// Safety-point and ownership helpers.
using namespace pause_examples;

// A business path owns its pause responsibility and retires its display scope.
// A moved-from Branch never updates the display again.
class Branch {
    ProgressStore* display_;
    const Flow* scope_;
public:
    PauseBranch pause;
    Branch(PauseBranch branch, ProgressStore& display, const Flow& scope)
        : display_(&display), scope_(&scope), pause(std::move(branch)) {}
    Branch(Branch&& other) noexcept
        : display_(std::exchange(other.display_, nullptr)), scope_(other.scope_), pause(std::move(other.pause)) {}
    ~Branch() { if (display_) display_->retire(*scope_); }
};

// Transparent UI observer around an existing sender; no business policy.
template<class Sender>
struct ObservedSender {
    using sender_concept = stdexec::sender_tag;
    Sender operation;
    ProgressStore* display;
    int id;
    auto get_env() const noexcept { return stdexec::get_env(operation); }
    template<class, class... Env>
    static consteval auto get_completion_signatures() noexcept
    { return stdexec::get_completion_signatures<Sender, Env...>(); }
    template<class Receiver>
    struct Observer {
        using receiver_concept = stdexec::receiver_tag;
        Receiver receiver;
        ProgressStore* display;
        int id;
        auto get_env() const noexcept { return stdexec::get_env(receiver); }
        template<class... Values>
        void set_value(Values&&... values) noexcept
        {
            display->set(id, NodeStatus::Completed);
            stdexec::set_value(std::move(receiver), std::forward<Values>(values)...);
        }
        template<class Error>
        void set_error(Error&& error) noexcept
        {
            display->set(id, NodeStatus::Failed);
            stdexec::set_error(std::move(receiver), std::forward<Error>(error));
        }
        void set_stopped() noexcept
        {
            const bool cancelled = stdexec::get_stop_token(get_env()).stop_requested();
            display->set(id, cancelled ? NodeStatus::Cancelled : NodeStatus::Stopped);
            stdexec::set_stopped(std::move(receiver));
        }
    };
    template<class Receiver>
    auto connect(Receiver receiver) &&
    {
        return stdexec::connect(std::move(operation), Observer<Receiver>{std::move(receiver), display, id});
    }
};
// The business coroutine borrows these safety-point and reporting functions.
// The UI-owned Session controls PauseSource and the execution lifetime.
class RunContext {
    ProgressStore& display_;
    exec::timed_thread_scheduler timer_;
    stdexec::task<void> point(Branch& branch, int id)
    {
        display_.set(id, NodeStatus::Waiting);
        co_await branch.pause.suspend_point({static_cast<std::size_t>(id)});
    }
public:
    static constexpr int All = -1;
    static constexpr int Skip = -2;
    RunContext(ProgressStore& display, exec::timed_thread_scheduler timer)
        : display_(display), timer_(timer) {}
    auto timer() const noexcept { return timer_; }
    // Wrap a sender: value arguments, errors and stopped are forwarded intact.
    // planned_ms == 0 means elapsed time only; no simulated delay is inserted.
    template<class Sender>
    auto step(Branch& branch, const std::string& key, Sender operation, int planned_ms = 0)
    {
        const int id = display_.graph().find(key);
        require(display_.graph().nodes[id].kind == NodeKind::Work, "step key must name a work node");
        display_.plan(id, planned_ms);
        // Construct the safety-point task when this sender actually starts.
        return stdexec::just()
            | stdexec::let_value([this, &branch, id] { return point(branch, id); })
            | stdexec::let_value(
                [this, id, operation = std::move(operation)]() mutable {
                    display_.set(id, NodeStatus::Running);
                    return ObservedSender<Sender>{std::move(operation), &display_, id};
                });
    }
    // Optional random-delay placeholder for a runnable UI preview.
    auto delay_step(Branch& branch, const std::string& key)
    {
        const int duration = display_.random_duration();
        return step(branch, key, exec::schedule_after(timer_, Milliseconds{duration}), duration);
    }
    // Select All, Skip, or one child index.
    // Build returns an ordinary sender, typically stdexec::when_all(...).
    template<class Build>
    stdexec::task<void> group(Branch& parent, std::string key, std::size_t count, Build build, int selection = All)
    {
        const auto& scope = display_.scope(key);
        require(scope.operation != Operation::Step && scope.operation != Operation::Sequence,
            "group key must name a fork/join scope");
        require(selection == All || selection == Skip
            || (selection >= 0 && static_cast<std::size_t>(selection) < scope.children.size()),
            "group selection is out of range");
        co_await point(parent, scope.entry);
        display_.set(scope.entry, NodeStatus::Completed);
        display_.set(scope.exit, NodeStatus::Joining, "等待所有子路径（包含子路径暂停时间）");
        std::vector<const Flow*> paths;
        for (int i = 0; i < static_cast<int>(scope.children.size()); ++i)
            if (selection == All || selection == i) paths.push_back(&scope.children[i]);
            else display_.skip(scope.children[i]);
        if (selection == Skip) count = 0;
        require(paths.size() == count, "business fork width differs from UI metadata");
        if (selection != Skip) {
            std::vector<Branch> children;
            if (!paths.empty()) {
                auto branches = take(parent.pause.fork_and_block_self(count));
                children.reserve(paths.size());
                for (int i = 0; i < static_cast<int>(paths.size()); ++i)
                    children.emplace_back(std::move(branches[i]), display_, *paths[i]);
            }
            co_await build(children);
        }
        auto note = selection == Skip ? "条件不满足，已跳过"
            : paths.empty() ? "无任务，未调用 fork(0)" : display_.note(scope.exit);
        if (note == "等待所有子路径（包含子路径暂停时间）") note = "全部子路径已完成";
        if (!paths.empty()) co_await point(parent, scope.exit);
        display_.set(scope.exit, NodeStatus::Completed, note);
    }
    void annotate(const std::string& group, std::string note)
    {
        display_.annotate(display_.scope(group).exit, std::move(note));
    }
};

// A layout and a coroutine factory: each start constructs a fresh task.
struct Workflow {
    Flow layout;
    std::function<stdexec::task<void>(RunContext&, Branch&)> run;
};
class Session {
    Workflow workflow_;
    PauseSource source_{AdvanceMode::SingleStep, []() noexcept {}};
    stdexec::inplace_stop_source stop_;
    ProgressStore display_;
    std::jthread host_;
    stdexec::task<void> execute(RunContext& work, Branch root)
    {
        co_await workflow_.run(work, root);
    }
    void run() noexcept
    {
        Completion completion = Completion::Error;
        std::string error;
        try {
            {
                exec::timed_thread_context timer;
                RunLoopThread executor;
                RunContext work(display_, timer.get_scheduler());
                auto root = Branch(take(source_.make_root_branch()), display_, graph().root);
                const auto result = stdexec::sync_wait(
                    stdexec::starts_on(executor.get_scheduler(), execute(work, std::move(root)))
                    | stdexec::write_env(stdexec::prop{stdexec::get_stop_token, stop_.get_token()}));
                completion = result ? Completion::Value : Completion::Stopped;
            }
        } catch (const std::exception& failure) { error = failure.what(); }
        catch (...) { error = "Unknown workflow failure"; }
        const auto drained = source_.snapshot();
        if (drained.live_branch_count || drained.waiting_branch_count) {
            completion = Completion::Error; error = "Workflow retained pause responsibilities after join";
        }
        display_.finish(completion, std::move(error));
    }
public:
    explicit Session(Workflow workflow, Options options = {})
        : workflow_(std::move(workflow)), display_(workflow_.layout, options)
    {
        if (!workflow_.run) throw std::invalid_argument("workflow requires a run function");
        if (options.pre_stopped) cancel();
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() { cancel(); if (host_.joinable()) host_.join(); }
    void start()
    {
        if (host_.joinable()) throw std::logic_error("session already started; create a fresh session to restart");
        host_ = std::jthread([this] { run(); });
    }
    const Graph& graph() const noexcept { return display_.graph(); }
    PauseSnapshot snapshot() const noexcept { return source_.snapshot(); }
    Progress progress() const { return display_.snapshot(); }
    auto pause() noexcept { return source_.request_pause(); }
    std::expected<void, PauseError> advance(AdvanceMode mode) noexcept
    {
        const auto state = snapshot();
        if (state.phase != GatePhase::Paused || !state.paused_epoch || !state.waiting_branch_count)
            return std::unexpected(PauseError::NotPaused);
        return source_.resume(*state.paused_epoch, mode);
    }
    void cancel() noexcept { (void)stop_.request_stop(); }
    PauseSource& source() noexcept { return source_; }
};
} // namespace pause_graph
#endif
