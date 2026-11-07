/* 测试项目：pause_state_model_test
 * 目的: 将 docs/design.md 的状态规则编码为可执行模型, 检查事件级不变量。
 * 测试项:
 * 1. API 错误优先级及失败不变性、空屏障、epoch/SingleStep、构造期取消、Resume/Stop 双方获胜。
 * 2. 分批注销先于交付、恢复调度 value/stopped、重叠暂停、创建即负有责任和 RAII 退役。
 * 3. 链/二叉/梳状多层 fork、直接子计数、父分支重入与重复 fork、
 *    多控制者旧 epoch 竞争、深层父分支重入前预取消。
 * 4. 独立重建计数/所有权关系, 拒绝 7 种故意缺陷, 执行有界状态搜索和固定种子随机事件。
 * 5. 可选深链模型检查暂停/退役/重入/复用及最终完全排空。
 * 预期: 每个事件保持计数、完成权、waiter、epoch 和树关系一致; 非法调用返回指定错误;
 *       注入缺陷必须被拒绝, 完整 scope 清空 records/operations/batches/waiters。
 * 入口: 配置 PAUSE_BUILD_DESIGN_TESTS=ON, CTest 名称 pause_state_model_test;
 *       pause_deep_chain_model_test 另执行 --chain-depth 16 --chain-only 1。
 *       CLI 默认 --depth 7 --random-steps 10000 --seeds 20 --max-records 16;
 *       可加 --chain-depth N (1..4096), --chain-only 1 必须同时指定 chain-depth。
 *       --depth 是搜索事件深度, 与 --chain-depth 的分支层数不同; 常规验证使用有限参数。
 * 边界: 不包含 pause.hpp、真实线程、C++ 对象生命周期或内存序验证。
 *       模型事件数不等于真实 Pause 场景/完整 scope 数, 有界探索不代表穷尽全部可能执行。
 */
// Executable design model for docs/design.md sections 4, 5, 7 and 8.
// This does NOT exercise pause.hpp, C++ object lifetimes, or actual threads.
// Each action is one lock transaction or one explicitly separate delivery step.
// The oracle reconstructs counts/ownership from records and checks event-level
// postconditions; directed mutation checks establish that it rejects key bugs.
#include <algorithm>
#include <cstdint>
#include <deque>
#include <iostream>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

enum class Gate { Open, Collecting, Paused };
enum class Phase { Registering, CancelRequested, Waiting, ValueClaimed,
                   StoppedClaimed, Scheduling, Complete };
enum class Owner { None, Start, Callback, Resume };
enum class Route { None, Inline, Resumed, Stopped };
enum class Mutation { None, MissingSchedulerWithdraw, DanglingWaiter,
                      AcceptStaleEpoch, DoubleDelivery, MidForkCommit,
                      MissingParentReturn, PrematureParentChildRelease };
enum class Kind { Root, Independent, Pause, ResumeContinuous, ResumeStep,
                  Begin, Constructed, Start, Stop, Reset, Deliver,
                  ScheduleValue, ScheduleStopped, Reap, Fork, Retire };

struct Action {
    Kind kind;
    int id = 0;
    std::uint64_t argument = 0;
};

const char* name(Kind kind) {
    switch (kind) {
    case Kind::Root: return "root";
    case Kind::Independent: return "independent";
    case Kind::Pause: return "request_pause";
    case Kind::ResumeContinuous: return "resume_continuous";
    case Kind::ResumeStep: return "resume_single_step";
    case Kind::Begin: return "register_callback";
    case Kind::Constructed: return "callback_constructor_return";
    case Kind::Start: return "start_decision";
    case Kind::Stop: return "callback";
    case Kind::Reset: return "reset_callback";
    case Kind::Deliver: return "raw_deliver";
    case Kind::ScheduleValue: return "scheduler_value";
    case Kind::ScheduleStopped: return "scheduler_stopped";
    case Kind::Reap: return "destroy_completed_operation";
    case Kind::Fork: return "fork";
    case Kind::Retire: return "retire_branch";
    }
    return "unknown";
}

std::string describe(Action a) {
    return std::string(name(a.kind)) + "(" + std::to_string(a.id) + ","
        + std::to_string(a.argument) + ")";
}

struct Branch {
    bool participates = true;
    bool pending = false;
    int waiter = -1;
    int children = 0;
    int parent = -1;
};

struct Operation {
    int branch;
    Phase phase = Phase::Registering;
    Owner owner = Owner::None;
    Route route = Route::None;
    bool callback_live = true;
    bool constructor_returned = false;
    bool stop_called = false;
    int claims = 0;
    int raw_deliveries = 0;
    int public_completions = 0;
    int batch = -1;
};

struct Model {
    bool initial_step = false;
    bool root_created = false;
    Gate gate = Gate::Open;
    std::uint64_t revision = 0;
    std::uint64_t epoch = 0;
    int origin = 0; // 0 absent, 1 control request, 2 single step.
    int round = -1;
    int live = 0;
    int pending = 0;
    int next_branch = 0;
    int next_operation = 0;
    int next_batch = 0;
    std::map<int, Branch> branches;
    std::map<int, Operation> operations;
    std::map<int, std::vector<int>> batches;
    std::vector<int> waiters;
    Mutation mutation = Mutation::None;

    explicit Model(bool step = false) : initial_step(step) {}

    void commit() {
        ++revision;
        if (gate == Gate::Collecting && pending == 0) {
            gate = Gate::Paused;
            epoch = revision;
        }
    }

    void collect(int new_origin) {
        require(gate == Gate::Open, "collect must start in Open");
        gate = Gate::Collecting;
        origin = new_origin;
        pending = live;
        for (auto& [id, branch] : branches) {
            (void) id;
            branch.pending = branch.participates;
        }
    }

    void arrive(Branch& branch) {
        if (branch.pending) {
            require(gate == Gate::Collecting && pending > 0,
                    "arrival outside Collecting");
            branch.pending = false;
            --pending;
        }
    }

    bool withdraw(Branch& branch) {
        require(branch.waiter == -1, "withdraw before unlink");
        if (!branch.participates) return false;
        arrive(branch);
        branch.participates = false;
        --live;
        return true;
    }

    bool idle(int branch_id) const {
        return std::none_of(operations.begin(), operations.end(),
            [branch_id](const auto& pair) { return pair.second.branch == branch_id; });
    }

    void claim(int id, Owner owner, Route route) {
        auto& op = operations.at(id);
        require(op.claims == 0, "second claimant");
        if (op.phase == Phase::Waiting) {
            std::erase(waiters, id);
            if (mutation != Mutation::DanglingWaiter)
                branches.at(op.branch).waiter = -1;
        }
        op.phase = route == Route::Stopped ? Phase::StoppedClaimed : Phase::ValueClaimed;
        op.owner = owner;
        op.route = route;
        ++op.claims;
    }

    bool can_deliver(int id) const {
        const auto& op = operations.at(id);
        if (op.phase != Phase::ValueClaimed && op.phase != Phase::StoppedClaimed)
            return false;
        if (op.owner == Owner::Callback) return true;
        if (op.callback_live || !op.constructor_returned) return false;
        if (op.owner == Owner::Resume) {
            for (int member : batches.at(op.batch))
                if (operations.at(member).callback_live) return false;
        }
        return true;
    }

    std::string perform(Action a) {
        switch (a.kind) {
        case Kind::Root: {
            if (root_created) return "RootAlreadyCreated";
            root_created = true;
            branches.emplace(next_branch++, Branch{});
            ++live;
            if (initial_step) { round = 0; collect(2); }
            commit();
            break;
        }
        case Kind::Independent: {
            if (!root_created) return "RootNotCreated";
            if (gate != Gate::Open) return "InvalidBranchState";
            branches.emplace(next_branch++, Branch{});
            ++live;
            commit();
            break;
        }
        case Kind::Pause: {
            if (!root_created) return "RootNotCreated";
            if (gate == Gate::Collecting) return "PauseAlreadyCollecting";
            if (gate == Gate::Paused) return "AlreadyPaused";
            collect(1);
            commit();
            break;
        }
        case Kind::ResumeContinuous:
        case Kind::ResumeStep: {
            if (!root_created) return "RootNotCreated";
            if (gate != Gate::Paused) return "NotPaused";
            if (epoch != a.argument && mutation != Mutation::AcceptStaleEpoch)
                return "StalePauseEpoch";
            auto batch = waiters;
            const int batch_id = next_batch++;
            for (int id : batch) {
                claim(id, Owner::Resume, Route::Resumed);
                operations.at(id).batch = batch_id;
            }
            if (!batch.empty()) batches.emplace(batch_id, std::move(batch));
            epoch = 0;
            gate = Gate::Open;
            if (a.kind == Kind::ResumeContinuous) { origin = 0; round = -1; }
            else { round = std::max(round, 0) + 1; collect(2); }
            commit();
            break;
        }
        case Kind::Begin: {
            auto& branch = branches.at(a.id);
            require(idle(a.id) && branch.children == 0 && branch.waiter == -1,
                    "concurrent/reentrant branch use");
            require(gate != Gate::Paused || !branch.participates,
                    "participating branch must already wait in Paused");
            operations.emplace(next_operation++, Operation{a.id});
            break;
        }
        case Kind::Constructed: {
            auto& op = operations.at(a.id);
            require(!op.constructor_returned, "callback constructed twice");
            op.constructor_returned = true;
            break;
        }
        case Kind::Start: {
            auto& op = operations.at(a.id);
            auto& branch = branches.at(op.branch);
            require(op.constructor_returned, "start before constructor return");
            require(op.phase == Phase::Registering || op.phase == Phase::CancelRequested,
                    "start decided twice");
            bool changed = false;
            if (op.phase == Phase::CancelRequested) {
                claim(a.id, Owner::Start, Route::Stopped);
                changed = withdraw(branch);
            } else {
                const bool participated = branch.participates;
                if (!participated) { branch.participates = true; ++live; changed = true; }
                if (gate == Gate::Open) claim(a.id, Owner::Start, Route::Inline);
                else {
                    require(gate != Gate::Paused || !participated,
                            "new waiter for participating Paused branch");
                    require(gate != Gate::Collecting || !participated || branch.pending,
                            "participating arrival not pending");
                    branch.waiter = a.id;
                    waiters.push_back(a.id);
                    arrive(branch);
                    op.phase = Phase::Waiting;
                    changed = true;
                }
            }
            if (changed) commit();
            break;
        }
        case Kind::Stop: {
            auto& op = operations.at(a.id);
            require(op.callback_live && !op.stop_called, "callback invoked twice/after reset");
            op.stop_called = true;
            if (op.phase == Phase::Registering) op.phase = Phase::CancelRequested;
            else if (op.phase == Phase::Waiting) {
                claim(a.id, Owner::Callback, Route::Stopped);
                withdraw(branches.at(op.branch));
                const int parent = branches.at(op.branch).parent;
                if (mutation == Mutation::PrematureParentChildRelease && parent != -1)
                    --branches.at(parent).children;
                commit();
            }
            break;
        }
        case Kind::Reset: {
            auto& op = operations.at(a.id);
            require(op.callback_live && op.constructor_returned, "reset while constructing");
            require(op.owner == Owner::Start || op.owner == Owner::Resume,
                    "reset by non-owner");
            op.callback_live = false;
            break;
        }
        case Kind::Deliver: {
            require(can_deliver(a.id), "delivery before callback quiescence");
            auto& op = operations.at(a.id);
            op.raw_deliveries += mutation == Mutation::DoubleDelivery ? 2 : 1;
            if (op.batch != -1) {
                const int batch_id = op.batch;
                std::erase(batches.at(batch_id), a.id);
                if (batches.at(batch_id).empty()) batches.erase(batch_id);
                op.batch = -1;
            }
            if (op.route == Route::Resumed) op.phase = Phase::Scheduling;
            else {
                if (op.route == Route::Stopped && withdraw(branches.at(op.branch))) commit();
                op.phase = Phase::Complete;
                ++op.public_completions;
            }
            break;
        }
        case Kind::ScheduleValue:
        case Kind::ScheduleStopped: {
            auto& op = operations.at(a.id);
            require(op.phase == Phase::Scheduling, "scheduler completed without schedule");
            if (a.kind == Kind::ScheduleStopped
                && mutation != Mutation::MissingSchedulerWithdraw
                && withdraw(branches.at(op.branch))) commit();
            op.phase = Phase::Complete;
            ++op.public_completions;
            break;
        }
        case Kind::Reap: {
            require(operations.at(a.id).phase == Phase::Complete, "destroy incomplete operation");
            operations.erase(a.id);
            break;
        }
        case Kind::Fork: {
            if (a.argument == 0) return "InvalidForkCount";
            auto& parent = branches.at(a.id);
            if (!idle(a.id) || !parent.participates || parent.children != 0
                || parent.waiter != -1 || gate == Gate::Paused)
                return "InvalidBranchState";
            withdraw(parent);
            if (mutation == Mutation::MidForkCommit) commit();
            parent.children = static_cast<int>(a.argument);
            for (std::uint64_t i = 0; i < a.argument; ++i) {
                Branch child;
                child.parent = a.id;
                child.pending = gate == Gate::Collecting;
                branches.emplace(next_branch++, child);
                ++live;
                if (child.pending) ++pending;
            }
            commit();
            break;
        }
        case Kind::Retire: {
            auto& branch = branches.at(a.id);
            require(idle(a.id) && branch.children == 0, "retire borrowed parent/branch");
            withdraw(branch);
            if (branch.parent != -1) {
                auto& parent = branches.at(branch.parent);
                --parent.children;
                if (mutation != Mutation::MissingParentReturn && parent.children == 0) {
                    if (gate == Gate::Paused) { gate = Gate::Collecting; epoch = 0; }
                    parent.participates = true;
                    ++live;
                    if (gate == Gate::Collecting) { parent.pending = true; ++pending; }
                }
            }
            branches.erase(a.id);
            commit();
            break;
        }
        }
        return "OK";
    }

    // Independent oracle: reconstruct cardinalities and relations from the
    // records, never using the incremental accounting helper functions above.
    void check() const {
        int counted_live = 0, counted_pending = 0;
        std::set<int> linked(waiters.begin(), waiters.end());
        require(linked.size() == waiters.size(), "duplicate shared waiter");
        std::set<int> record_waiters;
        for (const auto& [id, branch] : branches) {
            counted_live += branch.participates;
            counted_pending += branch.pending;
            int counted_children = 0, borrowers = 0;
            for (const auto& [other_id, other] : branches) {
                (void) other_id;
                counted_children += other.parent == id;
            }
            for (const auto& [op_id, op] : operations) {
                (void) op_id;
                borrowers += op.branch == id;
            }
            require(counted_children == branch.children, "direct-child count mismatch");
            require(borrowers <= 1, "more than one operation borrows branch");
            require(branch.parent == -1 || branches.contains(branch.parent), "dangling parent");
            // Derive ancestry from the records, independently of live/children.
            // A self-consistent local child count alone cannot exclude cycles.
            int ancestor = branch.parent;
            std::size_t hops = 0;
            while (ancestor != -1) {
                require(ancestor != id && ++hops <= branches.size(), "cyclic parent chain");
                require(branches.contains(ancestor), "dangling ancestor");
                ancestor = branches.at(ancestor).parent;
            }
            require(!branch.children || !branch.participates, "delegating parent participates");
            require(branch.participates || (!branch.pending && branch.waiter == -1),
                    "inactive responsibility has pending/waiter");
            if (branch.waiter != -1) {
                require(operations.contains(branch.waiter), "dangling branch waiter");
                const auto& op = operations.at(branch.waiter);
                require(op.branch == id && op.phase == Phase::Waiting,
                        "branch waiter not Waiting");
                record_waiters.insert(branch.waiter);
            }
            if (gate == Gate::Collecting && branch.participates)
                require(branch.pending != (branch.waiter != -1),
                        "Collecting responsibility must be pending XOR waiting");
            if (gate == Gate::Paused && branch.participates)
                require(branch.waiter != -1, "Paused responsibility lacks waiter");
        }
        require(live == counted_live && live >= 0, "live responsibility count mismatch");
        require(pending == counted_pending && pending >= 0, "arrival count mismatch");
        require(linked == record_waiters, "shared and branch waiter links disagree");
        require(gate == Gate::Collecting || pending == 0, "pending outside Collecting");
        require(gate != Gate::Collecting || pending > 0, "Collecting should have converged");
        require(gate != Gate::Open || waiters.empty(), "Open has shared waiter");
        require((gate == Gate::Paused) == (epoch != 0), "epoch/gate mismatch");
        require(epoch <= revision, "epoch newer than revision");
        require(gate != Gate::Open || (origin == 0 && round == -1), "Open diagnostics not cleared");
        require(gate == Gate::Open || origin != 0, "closed gate lacks origin");
        std::set<int> batched;
        for (const auto& [id, batch] : batches) {
            require(!batch.empty(), "empty retained completion batch");
            for (int member : batch) {
                require(batched.insert(member).second, "operation in multiple batches");
                const auto& op = operations.at(member);
                require(op.batch == id && op.owner == Owner::Resume
                        && op.phase == Phase::ValueClaimed, "invalid private batch owner");
            }
        }
        for (const auto& [id, op] : operations) {
            require(branches.contains(op.branch), "operation borrows retired branch");
            require(op.claims >= 0 && op.claims <= 1, "non-unique completion owner");
            require(op.raw_deliveries >= 0 && op.raw_deliveries <= 1, "multiple raw completions");
            require(op.public_completions >= 0 && op.public_completions <= 1,
                    "multiple public completions");
            require(op.public_completions <= op.raw_deliveries
                    && op.raw_deliveries <= op.claims, "completion without claim");
            require((op.owner == Owner::None) == (op.claims == 0), "owner/claim mismatch");
            require((op.phase == Phase::Complete) == (op.public_completions == 1),
                    "public completion phase mismatch");
            require((op.phase == Phase::Waiting) == linked.contains(id), "Waiting link mismatch");
            require((op.batch != -1) == batched.contains(id), "private batch link mismatch");
            if (!op.constructor_returned)
                require(op.claims == 0 && op.callback_live,
                        "completion/reset during callback construction");
            if (op.phase == Phase::Scheduling)
                require(op.route == Route::Resumed && op.raw_deliveries == 1
                        && !op.callback_live, "incorrect scheduling route");
        }
    }

    std::string key() const {
        std::ostringstream out;
        out << initial_step << ',' << root_created << ',' << static_cast<int>(gate)
            << ',' << revision << ',' << epoch << ',' << origin << ',' << round
            << ',' << live << ',' << pending << ',' << next_branch << ','
            << next_operation << ',' << next_batch << ';';
        for (const auto& [id, b] : branches)
            out << id << ',' << b.participates << ',' << b.pending << ',' << b.waiter
                << ',' << b.children << ',' << b.parent << ';';
        out << '/';
        for (const auto& [id, op] : operations)
            out << id << ',' << op.branch << ',' << static_cast<int>(op.phase) << ','
                << static_cast<int>(op.owner) << ',' << static_cast<int>(op.route) << ','
                << op.callback_live << ',' << op.constructor_returned << ','
                << op.stop_called << ',' << op.claims << ',' << op.raw_deliveries << ','
                << op.public_completions << ',' << op.batch << ';';
        out << '/';
        for (int id : waiters) out << id << ',';
        out << '/';
        for (const auto& [id, batch] : batches) {
            out << id << ':';
            for (int op : batch) out << op << ',';
            out << ';';
        }
        return out.str();
    }
};

std::uint64_t checked_events = 0;

// Contract checks on individual events are deliberately outside Model. In
// particular, matching cached counts alone cannot catch a missing withdrawal.
std::string checked(Model& model, Action action) {
    const bool can_reject = action.kind == Kind::Root || action.kind == Kind::Independent
        || action.kind == Kind::Pause || action.kind == Kind::ResumeContinuous
        || action.kind == Kind::ResumeStep || action.kind == Kind::Fork;
    const std::string before = can_reject ? model.key() : std::string{};
    const auto old_gate = model.gate;
    const auto old_epoch = model.epoch;
    const auto old_revision = model.revision;
    const int old_live = model.live;
    const int old_pending = model.pending;
    int branch_id = -1;
    if (action.kind == Kind::ScheduleStopped || action.kind == Kind::Start
        || action.kind == Kind::Stop)
        branch_id = model.operations.at(action.id).branch;
    else if (action.kind == Kind::Retire || action.kind == Kind::Fork)
        branch_id = action.id;
    const Branch old_branch = branch_id == -1 ? Branch{} : model.branches.at(branch_id);
    const Branch old_parent = old_branch.parent == -1 ? Branch{}
        : model.branches.at(old_branch.parent);
    const bool reentry = action.kind == Kind::Start && !old_branch.participates
        && model.operations.at(action.id).phase != Phase::CancelRequested;
    const std::string result = model.perform(action);
    model.check();
    if (result != "OK") require(before == model.key(), "failed API mutated state");
    if (action.kind == Kind::ResumeContinuous || action.kind == Kind::ResumeStep) {
        if (!model.root_created) require(result == "RootNotCreated", "resume error priority");
        else if (old_gate != Gate::Paused) require(result == "NotPaused", "resume phase priority");
        else if (action.argument != old_epoch)
            require(result == "StalePauseEpoch", "stale resume accepted");
    }
    if (result == "OK" && action.kind == Kind::ScheduleStopped)
        require(!model.branches.at(branch_id).participates,
                "scheduler stopped retained effective responsibility");
    if (result == "OK" && action.kind == Kind::Fork) {
        require(model.revision == old_revision + 1, "fork committed more than once");
        require(model.live == old_live - 1 + static_cast<int>(action.argument),
                "fork effective count delta");
        if (old_gate == Gate::Collecting)
            require(model.pending == old_pending - 1 + static_cast<int>(action.argument),
                    "fork pending count delta");
    }
    if (result == "OK" && action.kind == Kind::Retire && old_branch.parent != -1) {
        const auto& parent = model.branches.at(old_branch.parent);
        require(parent.children == old_parent.children - 1,
                "record retirement did not release exactly one direct child");
        if (old_parent.children == 1) {
            require(parent.participates && parent.waiter == -1,
                    "last child failed to return parent responsibility");
            require(parent.pending == (old_gate != Gate::Open),
                    "returned parent must owe one arrival in a closed gate");
            require(model.live == old_live - old_branch.participates + 1,
                    "atomic child-to-parent live transfer mismatch");
            if (old_gate == Gate::Paused)
                require(model.gate == Gate::Collecting && model.epoch == 0,
                        "parent return retained a stale Paused epoch");
        } else {
            require(parent.participates == old_parent.participates
                    && parent.pending == old_parent.pending && parent.waiter == old_parent.waiter,
                    "parent restored before its last direct child retired");
        }
    }
    if (result == "OK" && reentry) {
        require(model.live == old_live + 1 && model.pending == old_pending,
                "parent reentry must admit and arrive in one transaction");
        require(model.branches.at(branch_id).participates,
                "parent reentry failed to restore responsibility");
    }
    if (result == "OK" && branch_id != -1 && action.kind != Kind::Retire
        && old_branch.parent != -1)
        require(model.branches.at(old_branch.parent).children == old_parent.children,
                "non-retirement event changed ancestor direct-child count");
    if (old_gate == Gate::Paused && model.gate == Gate::Paused
        && action.kind != Kind::ResumeStep)
        require(model.epoch == old_epoch, "same-round epoch changed");
    ++checked_events;
    return result;
}

void ok(Model& model, Action action) {
    const auto result = checked(model, action);
    if (result != "OK")
        throw std::runtime_error(std::string("action=") + name(action.kind)
            + " id=" + std::to_string(action.id) + " result=" + result
            + " gate=" + std::to_string(static_cast<int>(model.gate))
            + " pending=" + std::to_string(model.pending));
}

int begin_and_start(Model& model, int branch, bool pre_cancel = false) {
    const int id = model.next_operation;
    ok(model, {Kind::Begin, branch});
    if (pre_cancel) ok(model, {Kind::Stop, id});
    ok(model, {Kind::Constructed, id});
    ok(model, {Kind::Start, id});
    return id;
}

void finish_raw(Model& model, int id) {
    const auto owner = model.operations.at(id).owner;
    if (owner != Owner::Callback && model.operations.at(id).callback_live)
        ok(model, {Kind::Reset, id});
    ok(model, {Kind::Deliver, id});
}

void expect_error(Model& model, Action action, const char* expected) {
    require(checked(model, action) == expected, "unexpected error code");
}

int branch_depth(const Model& model, int id) {
    int depth = 0;
    for (int parent = model.branches.at(id).parent; parent != -1;
         parent = model.branches.at(parent).parent)
        ++depth;
    return depth;
}

const char* name(Gate gate) {
    switch (gate) {
    case Gate::Open: return "Open";
    case Gate::Collecting: return "Collecting";
    case Gate::Paused: return "Paused";
    }
    return "unknown";
}

enum class Shape { Chain, Binary, Comb };

const char* name(Shape shape) {
    switch (shape) {
    case Shape::Chain: return "chain";
    case Shape::Binary: return "binary";
    case Shape::Comb: return "mixed_width_comb";
    }
    return "unknown";
}

struct TreePlan {
    std::vector<int> leaves;
    std::vector<int> parents;
    std::map<int, std::vector<int>> children;
};

void build_tree(Model& model, int parent, Shape shape, int remaining,
                int level, TreePlan& plan) {
    const int count = shape == Shape::Chain ? 1
        : shape == Shape::Binary ? 2 : 2 + level % 2;
    const int first = model.next_branch;
    ok(model, {Kind::Fork, parent, static_cast<std::uint64_t>(count)});
    plan.parents.push_back(parent);
    auto& children = plan.children[parent];
    for (int i = 0; i < count; ++i) children.push_back(first + i);
    for (int child : children) {
        if (remaining > 1 && (shape == Shape::Binary || child == first))
            build_tree(model, child, shape, remaining - 1, level + 1, plan);
        else
            plan.leaves.push_back(child);
    }
}

struct NestedStats {
    std::uint64_t scenarios = 0;
    std::uint64_t events = 0;
    std::size_t peak_records = 0;
    std::map<std::pair<Shape, int>, std::uint64_t> profiles;
    std::map<Gate, std::uint64_t> retirement_gates;

    void print(const char* label) const {
        std::cout << label << ": complete_scenarios=" << scenarios
                  << " checked_events=" << events << " peak_records=" << peak_records << '\n';
        for (const auto& [profile, count] : profiles)
            std::cout << "  shape=" << name(profile.first) << " fork_depth=" << profile.second
                      << " scenarios=" << count << '\n';
        for (const auto& [gate, count] : retirement_gates)
            std::cout << "  retirement_gate=" << name(gate) << " scenarios=" << count << '\n';
    }
};

// Each invocation is a complete, drained scope, unlike one random event. The
// plan is built separately from the model's cached child/live/pending counters.
// The independent anchor controls the gate while nested records are retired.
void nested_scenario(Shape shape, int depth, Gate retirement_gate,
                     int variation, bool pause_first, NestedStats& stats) {
    const auto events_before = checked_events;
    Model model;
    ok(model, {Kind::Root});
    ok(model, {Kind::Independent}); // Independent cancellation scope / anchor.
    if (pause_first) ok(model, {Kind::Pause});
    TreePlan plan;
    build_tree(model, 0, shape, depth, 0, plan);
    stats.peak_records = std::max(stats.peak_records, model.branches.size());
    require(model.live == static_cast<int>(plan.leaves.size()) + 1,
            "nested tree responsibility is not its leaves plus the anchor");
    for (const auto& [parent, children] : plan.children) {
        require(model.branches.at(parent).children == static_cast<int>(children.size()),
                "nested parent counts descendants instead of direct children");
        expect_error(model, {Kind::Fork, parent, 1}, "InvalidBranchState");
    }
    int actual_depth = 0;
    for (int leaf : plan.leaves) actual_depth = std::max(actual_depth, branch_depth(model, leaf));
    require(actual_depth == depth, "nested generator did not reach requested fork depth");
    if (!pause_first) ok(model, {Kind::Pause});
    require(model.pending == static_cast<int>(plan.leaves.size()) + 1,
            "pause/fork ordering changed initial arrival obligations");

    struct LeafOperation { int branch; int operation; int outcome; };
    std::vector<LeafOperation> leaf_operations;
    std::size_t index = 0;
    for (int leaf : plan.leaves) {
        const int outcome = (static_cast<int>(index++) + variation) % 5;
        if (outcome == 4) {
            // A prepared task can fail or exit before starting its first pause.
            ok(model, {Kind::Retire, leaf});
            continue;
        }
        const int operation = begin_and_start(model, leaf, outcome == 0);
        if (outcome == 1) ok(model, {Kind::Stop, operation});
        if (outcome <= 1) finish_raw(model, operation);
        // Keep completed operations/records alive until the later unwind.
        leaf_operations.push_back({leaf, operation, outcome});
    }
    // Early-retired leaves may already have returned an internal parent.
    // Those newly pending paths must also reach a point before resume.
    std::vector<int> restored_operations;
    for (const auto& [branch, record] : model.branches) {
        if (branch != 1 && record.participates && record.pending && model.idle(branch))
            restored_operations.push_back(begin_and_start(model, branch));
    }
    const int anchor = begin_and_start(model, 1);
    require(model.gate == Gate::Paused && model.pending == 0,
            "nested leaves did not converge before resumption");
    ok(model, {retirement_gate == Gate::Open ? Kind::ResumeContinuous : Kind::ResumeStep,
               0, model.epoch});

    std::vector<int> resumed;
    for (const auto& leaf : leaf_operations)
        if (leaf.outcome >= 2) resumed.push_back(leaf.operation);
    resumed.insert(resumed.end(), restored_operations.begin(), restored_operations.end());
    resumed.push_back(anchor);
    for (int operation : resumed) {
        if ((variation & 1) != 0) ok(model, {Kind::Stop, operation});
        ok(model, {Kind::Reset, operation});
    }
    // All callbacks in the private batch are quiescent before any completion.
    for (const auto& leaf : leaf_operations) {
        if (leaf.outcome < 2) continue;
        finish_raw(model, leaf.operation);
        ok(model, {retirement_gate == Gate::Paused || leaf.outcome == 2
                       ? Kind::ScheduleStopped : Kind::ScheduleValue, leaf.operation});
    }
    for (int operation : restored_operations) {
        finish_raw(model, operation);
        ok(model, {retirement_gate == Gate::Paused ? Kind::ScheduleStopped : Kind::ScheduleValue, operation});
        ok(model, {Kind::Reap, operation});
    }
    finish_raw(model, anchor);
    ok(model, {retirement_gate == Gate::Paused ? Kind::ScheduleStopped : Kind::ScheduleValue,
               anchor});
    require(model.gate == retirement_gate, "nested retirement gate was not established");

    for (auto leaf = leaf_operations.rbegin(); leaf != leaf_operations.rend(); ++leaf) {
        const int parent = model.branches.at(leaf->branch).parent;
        const int direct_children = model.branches.at(parent).children;
        ok(model, {Kind::Reap, leaf->operation});
        require(model.branches.at(parent).children == direct_children,
                "completed operation destruction retired its still-owned branch");
        ok(model, {Kind::Retire, leaf->branch});
    }
    for (auto parent = plan.parents.rbegin(); parent != plan.parents.rend(); ++parent) {
        require(model.branches.at(*parent).children == 0 && model.idle(*parent),
                "nested parent still owns live children or an operation");
        if (model.branches.at(*parent).participates && model.gate != Gate::Paused) {
            const int child = model.next_branch;
            ok(model, {Kind::Fork, *parent, 1});
            ok(model, {Kind::Retire, child});
        } else expect_error(model, {Kind::Fork, *parent, 1}, "InvalidBranchState");
        if (((*parent + variation) & 1) == 0) {
            const int previous_pending = model.pending;
            const auto previous_epoch = model.epoch;
            const auto previous_gate = model.gate;
            const bool owed_arrival = model.branches.at(*parent).pending;
            const int operation = begin_and_start(model, *parent);
            require(model.pending == previous_pending - owed_arrival,
                    "parent point failed to satisfy exactly its returned arrival");
            if (previous_gate == Gate::Paused)
                require(model.epoch == previous_epoch, "inactive reentry changed the existing epoch");
            if (retirement_gate != Gate::Open) ok(model, {Kind::Stop, operation});
            finish_raw(model, operation);
            ok(model, {Kind::Reap, operation});
            if (retirement_gate == Gate::Open) {
                // Non-consuming reuse is legal only after the reentry value.
                const int first = model.next_branch;
                ok(model, {Kind::Fork, *parent, 2});
                stats.peak_records = std::max(stats.peak_records, model.branches.size());
                ok(model, {Kind::Retire, first + 1});
                ok(model, {Kind::Retire, first});
            }
        }
        ok(model, {Kind::Retire, *parent});
        require((model.gate == Gate::Open) == (retirement_gate == Gate::Open),
                "nested unwind lost the pause intent");
    }
    ok(model, {Kind::Reap, anchor});
    ok(model, {Kind::Retire, 1});
    require(model.branches.empty() && model.operations.empty() && model.batches.empty()
            && model.waiters.empty() && model.live == 0 && model.pending == 0,
            "nested scope did not fully release its records and operations");
    require(model.gate == (retirement_gate == Gate::Open ? Gate::Open : Gate::Paused),
            "nested empty scope lost its pause intent");
    ++stats.scenarios;
    stats.events += checked_events - events_before;
    ++stats.profiles[{shape, actual_depth}];
    ++stats.retirement_gates[retirement_gate];
}

int nested_directed_tests() {
    NestedStats stats;
    for (Shape shape : {Shape::Chain, Shape::Binary, Shape::Comb}) {
        for (int depth : {1, 2, 4, 8}) {
            // A complete depth-eight binary tree is unnecessary here: depth
            // and fan-out are independently covered without 511 live records.
            if (shape == Shape::Binary && depth == 8) continue;
            for (Gate gate : {Gate::Open, Gate::Collecting, Gate::Paused})
                for (int variation = 0; variation < 5; ++variation)
                    nested_scenario(shape, depth, gate, variation, (variation & 1) != 0, stats);
        }
    }
    stats.print("directed_nested_model");
    int passed = static_cast<int>(stats.scenarios);
    // Both lock orders between deep ancestor reentry and Resume, in both modes.
    for (int depth : {4, 8}) {
        for (bool resume_first : {false, true}) {
            for (bool step : {false, true}) {
                Model model(true); ok(model, {Kind::Root});
                TreePlan plan; build_tree(model, 0, Shape::Chain, depth, 0, plan);
                for (int branch = model.next_branch - 1; branch > 0; --branch)
                    ok(model, {Kind::Retire, branch});
                // Explicitly accept cancellation of the returned root. Only a
                // withdrawn branch may rejoin an already-Paused epoch.
                const int cancelled = begin_and_start(model, 0, true);
                finish_raw(model, cancelled); ok(model, {Kind::Reap, cancelled});
                const auto old_epoch = model.epoch;
                int operation = -1;
                if (!resume_first) operation = begin_and_start(model, 0);
                ok(model, {step ? Kind::ResumeStep : Kind::ResumeContinuous, 0, old_epoch});
                if (resume_first) operation = begin_and_start(model, 0);
                if (resume_first && step) {
                    require(model.epoch > old_epoch && model.operations.at(operation).phase == Phase::Waiting,
                            "late ancestor did not join the new empty-step epoch");
                    ok(model, {Kind::ResumeContinuous, 0, model.epoch});
                }
                const Route route = model.operations.at(operation).route;
                require(route == (resume_first && !step ? Route::Inline : Route::Resumed),
                        "ancestor reentry/Resume lock order chose the wrong route");
                finish_raw(model, operation);
                if (route == Route::Resumed) ok(model, {Kind::ScheduleValue, operation});
                ok(model, {Kind::Reap, operation});
                const int first = model.next_branch;
                ok(model, {Kind::Fork, 0, 2}); // Also exercises repeat fork in Collecting.
                ok(model, {Kind::Retire, first}); ok(model, {Kind::Retire, first + 1});
                ok(model, {Kind::Retire, 0});
                require(model.branches.empty() && model.operations.empty(),
                        "ancestor reentry/Resume scenario leaked ownership");
                ++passed;
            }
        }
    }
    return passed;
}

int nested_overlap_tests() {
    int controller_scenarios = 0;
    // Two controllers hold the same old epoch while a deep ancestor reenters.
    // Enumerate all lock orders, including an empty SingleStep creating a new
    // epoch before reentry and before the losing controller submits its command.
    for (int depth : {4, 8}) {
        for (bool step : {false, true}) {
            std::vector<int> order{0, 1, 2}; // 0: reentry; 1/2: controllers.
            do {
                Model model(true); ok(model, {Kind::Root});
                TreePlan plan; build_tree(model, 0, Shape::Chain, depth, 0, plan);
                for (int branch = model.next_branch - 1; branch > 0; --branch)
                    ok(model, {Kind::Retire, branch});
                // Explicitly accept cancellation of the returned root. Only a
                // withdrawn branch may rejoin an already-Paused epoch.
                const int cancelled = begin_and_start(model, 0, true);
                finish_raw(model, cancelled); ok(model, {Kind::Reap, cancelled});
                const auto old_epoch = model.epoch;
                int operation = -1, successful_resumes = 0;
                bool reentered_before_resume = false;
                for (int event : order) {
                    if (event == 0) {
                        operation = begin_and_start(model, 0);
                    } else {
                        const Action resume{step ? Kind::ResumeStep : Kind::ResumeContinuous,
                                            0, old_epoch};
                        if (successful_resumes == 0) {
                            reentered_before_resume = operation != -1;
                            ok(model, resume);
                            ++successful_resumes;
                        } else {
                            expect_error(model, resume,
                                step && !reentered_before_resume ? "StalePauseEpoch" : "NotPaused");
                        }
                    }
                }
                require(successful_resumes == 1, "same epoch resumed twice");
                const Route expected_route = reentered_before_resume ? Route::Resumed
                    : step ? Route::None : Route::Inline;
                require(model.operations.at(operation).route == expected_route,
                        "competing controllers changed ancestor reentry route");
                if (expected_route == Route::None) {
                    require(model.gate == Gate::Paused && model.epoch > old_epoch,
                            "late ancestor did not join the new empty-step epoch");
                    ok(model, {Kind::ResumeContinuous, 0, model.epoch});
                }
                const Route route = model.operations.at(operation).route;
                finish_raw(model, operation);
                if (route == Route::Resumed) ok(model, {Kind::ScheduleValue, operation});
                ok(model, {Kind::Reap, operation}); ok(model, {Kind::Retire, 0});
                require(model.branches.empty() && model.operations.empty()
                        && model.batches.empty() && model.waiters.empty()
                        && model.live == 0 && model.pending == 0,
                        "competing controller scenario leaked nested ownership");
                ++controller_scenarios;
            } while (std::next_permutation(order.begin(), order.end()));
        }
    }
    // One side of a depth-three tree completes, rejoins and forks again while
    // the other side is still in the previous Resume's private completion batch.
    // The next pause also forks a pending child after another child has arrived.
    {
        Model model; ok(model, {Kind::Root});
        ok(model, {Kind::Fork, 0, 2});
        ok(model, {Kind::Fork, 1, 1}); ok(model, {Kind::Fork, 3, 1});
        ok(model, {Kind::Fork, 2, 1}); ok(model, {Kind::Fork, 5, 1});
        ok(model, {Kind::Pause});
        const int left = begin_and_start(model, 4), right = begin_and_start(model, 6);
        const auto old_epoch = model.epoch;
        ok(model, {Kind::ResumeContinuous, 0, old_epoch});
        ok(model, {Kind::Reset, left}); ok(model, {Kind::Reset, right});
        finish_raw(model, left); ok(model, {Kind::ScheduleValue, left});
        ok(model, {Kind::Reap, left}); ok(model, {Kind::Retire, 4});
        const int parent = begin_and_start(model, 3);
        finish_raw(model, parent); ok(model, {Kind::Reap, parent});
        ok(model, {Kind::Fork, 3, 2}); // New leaves 7 and 8.
        ok(model, {Kind::Pause});
        const int first = begin_and_start(model, 7);
        const auto revision_before_fork = model.revision;
        ok(model, {Kind::Fork, 8, 2}); // Leaves 9 and 10, with 7 already Waiting.
        require(model.revision == revision_before_fork + 1
                && model.pending == 3 && model.waiters.size() == 1,
                "Collecting fork disturbed an existing waiter or split its commit");
        const int second = begin_and_start(model, 9), third = begin_and_start(model, 10);
        require(branch_depth(model, 9) == 4 && model.pending == 1
                && model.gate == Gate::Collecting
                && model.operations.at(right).phase == Phase::ValueClaimed
                && model.batches.size() == 1,
                "undelivered old batch did not retain its next-round arrival debt");
        expect_error(model, {Kind::ResumeStep, 0, old_epoch}, "NotPaused");
        finish_raw(model, right); ok(model, {Kind::ScheduleStopped, right});
        require(model.gate == Gate::Paused && model.epoch > old_epoch
                && model.branches.at(5).children == 1 && model.batches.empty(),
                "old scheduler stopped failed to converge without retiring its record");
        ok(model, {Kind::Reap, right}); ok(model, {Kind::Retire, 6});
        require(model.gate == Gate::Collecting && model.epoch == 0,
                "right parent return failed to invalidate the old epoch");
        const int right_parent = begin_and_start(model, 5);
        const auto current_epoch = model.epoch;
        require(model.gate == Gate::Paused && model.waiters.size() == 4,
                "returned right parent failed to arrive at a fresh epoch");
        expect_error(model, {Kind::ResumeContinuous, 0, old_epoch}, "StalePauseEpoch");
        ok(model, {Kind::ResumeStep, 0, current_epoch});
        for (int operation : {first, second, third, right_parent})
            ok(model, {Kind::Reset, operation});
        for (int operation : {first, second, third, right_parent}) {
            finish_raw(model, operation);
            ok(model, {operation == second ? Kind::ScheduleValue : Kind::ScheduleStopped,
                       operation});
            ok(model, {Kind::Reap, operation});
        }
        require(model.gate == Gate::Collecting && model.pending == 1 && model.live == 1,
                "mixed next-round completions did not preserve the sole active leaf");
        for (int branch : {7, 9, 10, 8, 3, 1, 5, 2, 0})
            ok(model, {Kind::Retire, branch});
        require(model.branches.empty() && model.operations.empty() && model.batches.empty()
                && model.waiters.empty() && model.live == 0 && model.pending == 0
                && model.gate == Gate::Paused,
                "cross-round nested scope failed to drain while preserving pause intent");
    }
    std::cout << "nested_overlap_scenarios: competing_controllers=" << controller_scenarios
              << " cross_round_batches=1\n";
    return controller_scenarios + 1;
}

int nested_parent_precancel_tests() {
    int passed = 0;
    for (int depth : {1, 4, 8}) {
        for (Gate gate : {Gate::Open, Gate::Collecting, Gate::Paused}) {
            Model model;
            ok(model, {Kind::Root}); ok(model, {Kind::Independent});
            TreePlan plan;
            build_tree(model, 0, Shape::Chain, depth + 1, 0, plan);
            const int parent = plan.parents.back();
            require(branch_depth(model, parent) == depth,
                    "pre-cancelled parent did not reach requested depth");
            ok(model, {Kind::Retire, plan.leaves.front()});
            if (gate != Gate::Open) ok(model, {Kind::Pause});
            const int anchor = gate == Gate::Paused ? begin_and_start(model, 1) : -1;
            require(model.gate == (gate == Gate::Open ? Gate::Open : Gate::Collecting)
                    && model.branches.at(parent).participates
                    && model.branches.at(parent).children == 0,
                    "last child failed to return its pending parent");
            const auto revision = model.revision;
            const int live = model.live, pending = model.pending;
            const int ancestor = model.branches.at(parent).parent;
            const int ancestor_children = model.branches.at(ancestor).children;
            const int cancelled = begin_and_start(model, parent, true);
            finish_raw(model, cancelled);
            require(model.operations.at(cancelled).route == Route::Stopped
                    && model.operations.at(cancelled).public_completions == 1
                    && !model.branches.at(parent).participates
                    && model.branches.at(parent).waiter == -1,
                    "constructor cancellation failed to withdraw the returned parent");
            require(model.revision == revision + 1 && model.live == live - 1
                    && model.pending == pending - (gate == Gate::Open ? 0 : 1)
                    && model.branches.at(ancestor).children == ancestor_children,
                    "returned parent cancellation changed the wrong responsibility");
            ok(model, {Kind::Reap, cancelled});
            expect_error(model, {Kind::Fork, parent, 1}, "InvalidBranchState");
            const auto fresh_gate = model.gate;
            const auto cancelled_epoch = model.epoch;
            const int cancelled_pending = model.pending;
            const int fresh = begin_and_start(model, parent);
            require(model.live == live && model.pending == cancelled_pending
                    && model.epoch == cancelled_epoch && model.gate == fresh_gate,
                    "fresh branch reentry failed after accepted cancellation");
            if (gate != Gate::Open) ok(model, {Kind::Stop, fresh});
            finish_raw(model, fresh); ok(model, {Kind::Reap, fresh});
            for (auto it = plan.parents.rbegin(); it != plan.parents.rend(); ++it)
                ok(model, {Kind::Retire, *it});
            if (anchor != -1) {
                ok(model, {Kind::Stop, anchor});
                finish_raw(model, anchor); ok(model, {Kind::Reap, anchor});
            }
            ok(model, {Kind::Retire, 1});
            require(model.branches.empty() && model.operations.empty()
                    && model.batches.empty() && model.waiters.empty()
                    && model.live == 0 && model.pending == 0,
                    "pre-cancelled nested parent scope failed to drain");
            ++passed;
        }
    }
    std::cout << "nested_parent_precancel_scenarios=" << passed << '\n';
    return passed;
}

std::vector<Action> enabled(const Model& model, int max_records) {
    std::vector<Action> actions;
    if (!model.root_created) return {{Kind::Root}};
    if (model.gate == Gate::Open) {
        actions.push_back({Kind::Pause});
        if (static_cast<int>(model.branches.size()) < max_records)
            actions.push_back({Kind::Independent});
    } else if (model.gate == Gate::Paused) {
        actions.push_back({Kind::ResumeContinuous, 0, model.epoch});
        actions.push_back({Kind::ResumeStep, 0, model.epoch});
        actions.push_back({Kind::ResumeStep, 0, model.epoch + 1});
    }
    for (const auto& [id, branch] : model.branches) {
        if (!model.idle(id) || branch.children != 0) continue;
        actions.push_back({Kind::Retire, id});
        if (model.gate != Gate::Paused || !branch.participates)
            actions.push_back({Kind::Begin, id});
        if (branch.participates && model.gate != Gate::Paused) {
            for (std::uint64_t count = 1; count <= 3; ++count)
                if (model.branches.size() + count <= static_cast<std::size_t>(max_records))
                    actions.push_back({Kind::Fork, id, count});
        }
    }
    for (const auto& [id, op] : model.operations) {
        if (!op.constructor_returned) actions.push_back({Kind::Constructed, id});
        if (op.constructor_returned
            && (op.phase == Phase::Registering || op.phase == Phase::CancelRequested))
            actions.push_back({Kind::Start, id});
        if (op.callback_live && !op.stop_called) actions.push_back({Kind::Stop, id});
        if (op.callback_live && op.constructor_returned
            && (op.owner == Owner::Start || op.owner == Owner::Resume))
            actions.push_back({Kind::Reset, id});
        if (model.can_deliver(id)) actions.push_back({Kind::Deliver, id});
        if (op.phase == Phase::Scheduling) {
            actions.push_back({Kind::ScheduleValue, id});
            actions.push_back({Kind::ScheduleStopped, id});
        }
        if (op.phase == Phase::Complete) actions.push_back({Kind::Reap, id});
    }
    return actions;
}

int directed_tests() {
    int passed = 0;
    // Error priority and immutable failure paths.
    {
        Model m;
        expect_error(m, {Kind::Pause}, "RootNotCreated");
        expect_error(m, {Kind::Independent}, "RootNotCreated");
        expect_error(m, {Kind::ResumeStep, 0, 123}, "RootNotCreated");
        ok(m, {Kind::Root});
        expect_error(m, {Kind::Root}, "RootAlreadyCreated");
        expect_error(m, {Kind::ResumeStep}, "NotPaused");
        expect_error(m, {Kind::Fork, 0, 0}, "InvalidForkCount");
        ok(m, {Kind::Pause});
        expect_error(m, {Kind::Pause}, "PauseAlreadyCollecting");
        expect_error(m, {Kind::Independent}, "InvalidBranchState");
        begin_and_start(m, 0);
        expect_error(m, {Kind::Pause}, "AlreadyPaused");
        expect_error(m, {Kind::ResumeStep, 0, m.epoch + 1}, "StalePauseEpoch");
        ++passed;
    }
    // Empty barriers preserve pause intent; each empty step changes the epoch.
    {
        Model m;
        ok(m, {Kind::Root}); ok(m, {Kind::Retire, 0});
        require(m.gate == Gate::Open && m.live == 0, "retire should not pause Open");
        ok(m, {Kind::Pause});
        for (int i = 1; i <= 10; ++i) {
            auto old = m.epoch;
            ok(m, {Kind::ResumeStep, 0, old});
            require(m.gate == Gate::Paused && m.epoch > old && m.round == i,
                    "empty step did not create a new barrier");
        }
        ok(m, {Kind::ResumeContinuous, 0, m.epoch});
        require(m.round == -1 && m.origin == 0, "continuous diagnostics");
        ++passed;
    }
    // Cancel during construction only records a flag; no callback storage reset.
    for (bool step : {false, true}) {
        Model m(step); ok(m, {Kind::Root});
        ok(m, {Kind::Begin, 0}); ok(m, {Kind::Stop, 0});
        require(m.operations.at(0).claims == 0 && m.live == 1, "early cancel completion");
        ok(m, {Kind::Constructed, 0}); ok(m, {Kind::Start, 0});
        require(m.live == 0, "cancel must withdraw responsibility");
        finish_raw(m, 0); ok(m, {Kind::Reap, 0}); ok(m, {Kind::Retire, 0});
        ++passed;
    }
    // Start wins Open; a later stop and pause do not rewrite InlinePass.
    {
        Model m; ok(m, {Kind::Root}); const int op = begin_and_start(m, 0);
        ok(m, {Kind::Stop, op}); ok(m, {Kind::Pause}); finish_raw(m, op);
        require(m.operations.at(op).route == Route::Inline && m.pending == 1,
                "late pause/stop rewrote inline completion");
        ok(m, {Kind::Reap, op}); ok(m, {Kind::Retire, 0});
        require(m.gate == Gate::Paused, "retire did not satisfy pending");
        ++passed;
    }
    // Both winner orders, each mode, and both resumed scheduler outcomes.
    for (bool callback_first : {false, true}) {
        for (bool step : {false, true}) {
            for (bool scheduler_stops : {false, true}) {
                Model m(true); ok(m, {Kind::Root}); int op = begin_and_start(m, 0);
                if (callback_first) {
                    ok(m, {Kind::Stop, op});
                    ok(m, {step ? Kind::ResumeStep : Kind::ResumeContinuous, 0, m.epoch});
                    require(m.operations.at(op).owner == Owner::Callback, "cancel lost ownership");
                    finish_raw(m, op);
                } else {
                    ok(m, {step ? Kind::ResumeStep : Kind::ResumeContinuous, 0, m.epoch});
                    ok(m, {Kind::Stop, op});
                    require(m.operations.at(op).owner == Owner::Resume, "late stop stole ownership");
                    finish_raw(m, op);
                    ok(m, {scheduler_stops ? Kind::ScheduleStopped : Kind::ScheduleValue, op});
                    require(m.live == (scheduler_stops ? 0 : 1), "schedule responsibility outcome");
                    if (step && scheduler_stops)
                        require(m.gate == Gate::Paused && m.pending == 0,
                                "scheduler stopped stranded next step");
                }
                ++passed;
            }
        }
    }
    // Fork in Collecting is one atomic transfer, recursively counts direct children.
    {
        Model m; ok(m, {Kind::Root}); ok(m, {Kind::Pause}); ok(m, {Kind::Fork, 0, 2});
        require(m.live == 2 && m.pending == 2, "fork transfer counts");
        ok(m, {Kind::Fork, 1, 2});
        require(m.branches.at(0).children == 2 && m.branches.at(1).children == 2,
                "recursive fork changed grandparent direct count");
        ok(m, {Kind::Retire, 3}); ok(m, {Kind::Retire, 4});
        require(m.branches.at(0).children == 2 && m.branches.at(1).participates,
                "last child did not restore its parent");
        ok(m, {Kind::Retire, 1}); ok(m, {Kind::Retire, 2});
        require(m.gate == Gate::Collecting && m.live == 1 && m.pending == 1, "parent responsibility was not restored");
        auto epoch = m.epoch;
        begin_and_start(m, 0);
        require(m.epoch != epoch && m.gate == Gate::Paused && m.live == 1,
                "parent arrival did not establish a fresh Paused epoch");
        ++passed;
    }
    // Canceled child responsibility disappears before its record; parent stays blocked.
    {
        Model m(true); ok(m, {Kind::Root}); ok(m, {Kind::Fork, 0, 1});
        int op = begin_and_start(m, 1); ok(m, {Kind::Stop, op}); finish_raw(m, op);
        require(m.live == 0 && m.branches.at(0).children == 1, "cancel retired record early");
        ok(m, {Kind::Reap, op}); ok(m, {Kind::Retire, 1});
        require(m.branches.at(0).children == 0 && m.branches.at(0).participates,
                "record retirement failed to restore parent");
        ++passed;
    }
    // Last-child retirement returns a pending parent in every closed gate.
    for (Gate target : {Gate::Open, Gate::Collecting, Gate::Paused}) {
        Model m; ok(m, {Kind::Root}); ok(m, {Kind::Fork, 0, 1});
        if (target != Gate::Open) ok(m, {Kind::Pause});
        if (target == Gate::Paused) {
            int child = begin_and_start(m, 1);
            ok(m, {Kind::Stop, child}); finish_raw(m, child); ok(m, {Kind::Reap, child});
            require(m.gate == Gate::Paused, "cancelled child did not converge");
        }
        const auto old_epoch = m.epoch;
        ok(m, {Kind::Retire, 1});
        require(m.branches.at(0).participates && m.live == 1,
                "last child did not return its parent");
        require(m.gate == (target == Gate::Open ? Gate::Open : Gate::Collecting),
                "parent return did not restore closed-gate collection");
        require(m.epoch == 0 && m.pending == (target == Gate::Open ? 0 : 1),
                "returned parent count/epoch mismatch");
        const int op = begin_and_start(m, 0);
        require(m.operations.at(op).route == (target == Gate::Open ? Route::Inline : Route::None),
                "parent arrival routed incorrectly");
        if (target != Gate::Open)
            require(m.gate == Gate::Paused && m.epoch != old_epoch,
                    "parent arrival did not generate a fresh epoch");
        ++passed;
    }
    // Entire resumed batch is silent before its first completion. New pause can
    // run before delivery; resumed scheduling stopped must satisfy that new round.
    {
        Model m; ok(m, {Kind::Root}); ok(m, {Kind::Fork, 0, 2}); ok(m, {Kind::Pause});
        int a = begin_and_start(m, 1), b = begin_and_start(m, 2);
        ok(m, {Kind::ResumeContinuous, 0, m.epoch});
        ok(m, {Kind::Pause});
        ok(m, {Kind::Reset, a});
        require(!m.can_deliver(a), "first delivery allowed before whole batch reset");
        ok(m, {Kind::Stop, b}); ok(m, {Kind::Reset, b});
        ok(m, {Kind::Deliver, a}); ok(m, {Kind::ScheduleStopped, a});
        ok(m, {Kind::Deliver, b}); ok(m, {Kind::ScheduleStopped, b});
        require(m.gate == Gate::Paused && m.live == 0, "overlapping pause retained pending");
        ++passed;
    }
    // Responsibility exists at branch creation, before its first suspend point.
    {
        Model m; ok(m, {Kind::Root}); ok(m, {Kind::Fork, 0, 2}); ok(m, {Kind::Pause});
        begin_and_start(m, 2);
        require(m.gate == Gate::Collecting && m.pending == 1,
                "unstarted but registered child lost responsibility");
        ok(m, {Kind::Retire, 1});
        require(m.gate == Gate::Paused, "child RAII retirement did not converge");
        ++passed;
    }
    return passed;
}

int mutation_tests() {
    int rejected = 0;
    for (auto mutation : {Mutation::MissingSchedulerWithdraw, Mutation::DanglingWaiter,
                          Mutation::AcceptStaleEpoch, Mutation::DoubleDelivery,
                          Mutation::MidForkCommit, Mutation::MissingParentReturn,
                          Mutation::PrematureParentChildRelease}) {
        bool caught = false;
        try {
            Model m(mutation != Mutation::MissingParentReturn); ok(m, {Kind::Root});
            if (mutation == Mutation::MissingParentReturn) {
                ok(m, {Kind::Fork, 0, 1});
                m.mutation = mutation; ok(m, {Kind::Retire, 1});
            } else if (mutation == Mutation::PrematureParentChildRelease) {
                ok(m, {Kind::Fork, 0, 1});
                const int op = begin_and_start(m, 1);
                m.mutation = mutation; ok(m, {Kind::Stop, op});
            } else if (mutation == Mutation::MidForkCommit) {
                m.mutation = mutation; ok(m, {Kind::Fork, 0, 2});
            } else {
                int op = begin_and_start(m, 0);
                m.mutation = mutation;
                const auto credential = m.epoch + (mutation == Mutation::AcceptStaleEpoch ? 1 : 0);
                ok(m, {Kind::ResumeStep, 0, credential});
                finish_raw(m, op);
                ok(m, {Kind::ScheduleStopped, op});
            }
        } catch (const std::runtime_error&) { caught = true; }
        require(caught, "intentional mutation escaped the oracle");
        ++rejected;
    }
    return rejected;
}

void explore(int depth) {
    struct Node { Model model; int depth; std::string trace; };
    std::deque<Node> queue;
    std::set<std::string> seen;
    for (bool step : {false, true}) {
        Model m(step); m.check(); seen.insert(m.key());
        queue.push_back({std::move(m), 0, step ? "initial=SingleStep" : "initial=Continuous"});
    }
    std::uint64_t transitions = 0;
    while (!queue.empty()) {
        Node node = std::move(queue.front()); queue.pop_front();
        if (node.depth == depth) continue;
        for (Action action : enabled(node.model, 3)) {
            Model next = node.model;
            const std::string trace = node.trace + " -> " + describe(action);
            try { checked(next, action); }
            catch (const std::exception& error) {
                throw std::runtime_error(trace + "\n" + error.what());
            }
            ++transitions;
            if (seen.insert(next.key()).second)
                queue.push_back({std::move(next), node.depth + 1, trace});
        }
    }
    std::cout << "bounded_model: depth=" << depth << " max_records=3 states="
              << seen.size() << " transitions=" << transitions << '\n';
}

void random_runs(int seeds, int steps, int max_records) {
    std::uint64_t transitions = 0, resets = 0, complete_scopes = 0;
    std::uint64_t api_rejections = 0, nested_forks = 0, nested_cancellations = 0;
    std::size_t peak_records = 0;
    int max_fork_depth = 0, max_fork_width = 0;
    std::map<Kind, std::uint64_t> coverage;
    std::map<int, std::uint64_t> forks_by_depth;
    std::map<Gate, std::uint64_t> forks_by_gate, reentries_by_gate, retirements_by_gate;
    std::map<std::string, std::uint64_t> cancellations;
    constexpr std::uint64_t base_seed = 0x5041555345ULL;
    for (int seed_index = 0; seed_index < seeds; ++seed_index) {
        const auto seed = base_seed + static_cast<std::uint64_t>(seed_index);
        std::mt19937_64 rng(seed);
        Model model((seed_index % 2) != 0);
        std::deque<std::string> trace;
        for (int step = 0; step < steps; ++step) {
            // A finished empty scope is a fresh scenario, not an endless series
            // of empty pause/resume commands. Reset is not counted as an action.
            if (model.root_created && model.branches.empty()) {
                model = Model((rng() & 1) != 0); ++resets;
                trace.clear();
            }
            auto actions = enabled(model, max_records);
            require(!actions.empty(), "model has no enabled progress event");
            // Specified modulo mapping keeps event selection identical across
            // standard libraries; no implementation-defined distribution mapping.
            Action action = actions[static_cast<std::size_t>(rng() % actions.size())];
            const auto gate_before = model.gate;
            const int fork_depth = action.kind == Kind::Fork ? branch_depth(model, action.id) + 1 : 0;
            const bool reentry = action.kind == Kind::Start
                && !model.branches.at(model.operations.at(action.id).branch).participates
                && model.operations.at(action.id).phase != Phase::CancelRequested;
            std::string cancellation;
            int cancellation_depth = 0;
            if (action.kind == Kind::ScheduleStopped)
                cancellation = "scheduler_stopped";
            else if (action.kind == Kind::Start
                     && model.operations.at(action.id).phase == Phase::CancelRequested)
                cancellation = "constructor_cancel";
            else if (action.kind == Kind::Stop
                     && model.operations.at(action.id).phase == Phase::Waiting)
                cancellation = "waiting_cancel";
            if (!cancellation.empty())
                cancellation_depth = branch_depth(model, model.operations.at(action.id).branch);
            trace.push_back(describe(action));
            if (trace.size() > 40) trace.pop_front();
            std::string result;
            try { result = checked(model, action); }
            catch (const std::exception& error) {
                std::ostringstream message;
                message << "seed=" << seed << " event=" << step << "\n";
                for (const auto& item : trace) message << item << '\n';
                throw std::runtime_error(message.str() + error.what());
            }
            ++transitions; ++coverage[action.kind];
            if (result != "OK") ++api_rejections;
            if (action.kind == Kind::Fork && result == "OK") {
                max_fork_depth = std::max(max_fork_depth, fork_depth);
                max_fork_width = std::max(max_fork_width, static_cast<int>(action.argument));
                nested_forks += fork_depth >= 2;
                ++forks_by_depth[fork_depth];
                ++forks_by_gate[gate_before];
            }
            if (reentry) ++reentries_by_gate[gate_before];
            if (action.kind == Kind::Retire) {
                ++retirements_by_gate[gate_before];
                if (model.branches.empty()) ++complete_scopes;
            }
            if (!cancellation.empty()) {
                ++cancellations[cancellation];
                nested_cancellations += cancellation_depth >= 2;
            }
            peak_records = std::max(peak_records, model.branches.size());
        }
    }
    std::cout << "random_model: seed_base=" << base_seed << " seeds=" << seeds
              << " steps_per_seed=" << steps << " transitions=" << transitions
              << " successful_events=" << transitions - api_rejections
              << " api_rejections=" << api_rejections << " fresh_scopes=" << resets
              << " complete_scopes=" << complete_scopes << " event_kinds=" << coverage.size() << '\n';
    std::cout << "random_nested_coverage: configured_max_records=" << max_records
              << " peak_records=" << peak_records << " max_fork_depth=" << max_fork_depth
              << " max_fork_width=" << max_fork_width << " nested_forks=" << nested_forks
              << " nested_cancellations=" << nested_cancellations << '\n';
    for (const auto& [kind, count] : coverage)
        std::cout << "  " << name(kind) << '=' << count << '\n';
    for (const auto& [depth, count] : forks_by_depth)
        std::cout << "  forks_at_child_depth=" << depth << " count=" << count << '\n';
    for (const auto& [gate, count] : forks_by_gate)
        std::cout << "  fork_gate=" << name(gate) << " count=" << count << '\n';
    for (const auto& [gate, count] : reentries_by_gate)
        std::cout << "  reentry_gate=" << name(gate) << " count=" << count << '\n';
    for (const auto& [gate, count] : retirements_by_gate)
        std::cout << "  retirement_gate=" << name(gate) << " count=" << count << '\n';
    for (const auto& [path, count] : cancellations)
        std::cout << "  " << path << '=' << count << '\n';
}

// Explicit opt-in stress driver: construction and join unwinding are iterative,
// so the generator's call stack does not impose a fake fork-depth limit. Every
// transition still runs checked(), including the full existing record oracle.
// Its cost grows with retained records and ancestry; this is a bounded resource
// test, not evidence that an implementation supports arbitrary nesting.
void deep_chain_tests(int depth) {
    std::uint64_t complete_scopes = 0, total_events = 0, total_records = 0;
    std::size_t peak_records = 0;
    for (bool side_branches : {false, true}) {
        for (bool collecting_fork : {false, true}) {
            for (int outcome = 0; outcome != 3; ++outcome) {
                const char* outcome_name = outcome == 0 ? "constructor_cancel"
                    : outcome == 1 ? "waiting_cancel" : "scheduler_stopped";
                const auto events_before = checked_events;
                int level = 0;
                const char* stage = "build";
                try {
                    Model model;
                    ok(model, {Kind::Root});
                    if (collecting_fork) ok(model, {Kind::Pause});
                    std::vector<int> parents, sides;
                    int current = 0;
                    std::size_t scope_peak = 1;
                    for (; level != depth; ++level) {
                        parents.push_back(current);
                        const int first = model.next_branch;
                        ok(model, {Kind::Fork, current, side_branches ? 2U : 1U});
                        if (side_branches) sides.push_back(first + 1);
                        current = first;
                        scope_peak = std::max(scope_peak, model.branches.size());
                    }
                    const int actual_depth = branch_depth(model, current);
                    require(actual_depth == depth, "deep-chain generator missed requested depth");
                    if (!collecting_fork) ok(model, {Kind::Pause});
                    stage = "cancel_leaves";
                    std::vector<std::pair<int, int>> leaf_operations;
                    for (int side : sides)
                        leaf_operations.emplace_back(side, begin_and_start(model, side));
                    const int deepest = begin_and_start(model, current, outcome == 0);
                    leaf_operations.emplace_back(current, deepest);
                    require(model.gate == Gate::Paused && model.pending == 0,
                            "deep-chain leaves failed to converge");
                    if (outcome == 2) {
                        ok(model, {Kind::ResumeStep, 0, model.epoch});
                        // A batch spans side branches from every nesting level.
                        // Late callbacks lose, then all callbacks are reset before
                        // deepest-first delivery and scheduler cancellation.
                        for (const auto& [branch, operation] : leaf_operations) {
                            (void) branch;
                            ok(model, {Kind::Stop, operation});
                            ok(model, {Kind::Reset, operation});
                        }
                        for (auto leaf = leaf_operations.rbegin();
                             leaf != leaf_operations.rend(); ++leaf) {
                            finish_raw(model, leaf->second);
                            ok(model, {Kind::ScheduleStopped, leaf->second});
                        }
                    } else {
                        for (auto leaf = leaf_operations.rbegin();
                             leaf != leaf_operations.rend(); ++leaf) {
                            if (leaf->second != deepest || outcome == 1)
                                ok(model, {Kind::Stop, leaf->second});
                            finish_raw(model, leaf->second);
                        }
                    }
                    require(model.live == 0 && model.pending == 0
                            && model.gate == Gate::Paused,
                            "deep-chain cancellation left arrival debt");
                    for (const auto& [branch, operation] : leaf_operations) {
                        const int parent = model.branches.at(branch).parent;
                        const int children_before = model.branches.at(parent).children;
                        expect_error(model, {Kind::Fork, parent, 1}, "InvalidBranchState");
                        ok(model, {Kind::Reap, operation});
                        require(model.branches.at(parent).children == children_before,
                                "operation release retired a retained deep-chain record");
                    }
                    require(model.branches.size() == scope_peak,
                            "cancelled deep-chain records were released before join");
                    stage = "join_reentry_reuse";
                    for (level = depth - 1; level >= 0; --level) {
                        const int parent = parents[static_cast<std::size_t>(level)];
                        ok(model, {Kind::Retire, current});
                        if (side_branches) {
                            require(model.branches.at(parent).children == 1,
                                    "deep subtree retirement also released its direct sibling");
                            expect_error(model, {Kind::Fork, parent, 1}, "InvalidBranchState");
                            ok(model, {Kind::Retire, sides[static_cast<std::size_t>(level)]});
                        }
                        require(model.branches.at(parent).children == 0
                                && model.branches.at(parent).participates
                                && model.branches.at(parent).pending,
                                "deep-chain join failed to return one parent arrival");
                        const int immediate_child = model.next_branch;
                        ok(model, {Kind::Fork, parent, 1});
                        ok(model, {Kind::Retire, immediate_child});
                        int operation = begin_and_start(model, parent);
                        const auto join_epoch = model.epoch;
                        require(model.gate == Gate::Paused && join_epoch != 0,
                                "returned parent arrival did not establish a new epoch");
                        ok(model, {Kind::ResumeStep, 0, join_epoch});
                        finish_raw(model, operation);
                        ok(model, {Kind::ScheduleValue, operation});
                        ok(model, {Kind::Reap, operation});
                        require(model.gate == Gate::Collecting && model.pending == 1,
                                "SingleStep lost the reentered parent's next arrival");
                        operation = begin_and_start(model, parent);
                        require(model.gate == Gate::Paused && model.pending == 0,
                                "reentered parent did not reach its next SingleStep barrier");
                        ok(model, {Kind::ResumeContinuous, 0, model.epoch});
                        finish_raw(model, operation);
                        ok(model, {Kind::ScheduleValue, operation});
                        ok(model, {Kind::Reap, operation});
                        if (collecting_fork) ok(model, {Kind::Pause});
                        const int first = model.next_branch;
                        const int count = side_branches ? 2 : 1;
                        ok(model, {Kind::Fork, parent, static_cast<std::uint64_t>(count)});
                        scope_peak = std::max(scope_peak, model.branches.size());
                        for (int child = first + count - 1; child >= first; --child)
                            ok(model, {Kind::Retire, child});
                        if (!collecting_fork) ok(model, {Kind::Pause});
                        require(model.live == 1 && model.pending == 1
                                && model.gate == Gate::Collecting,
                                "deep-chain repeated fork lost its returned parent");
                        current = parent;
                    }
                    stage = "drain";
                    ok(model, {Kind::Retire, current});
                    require(model.branches.empty() && model.operations.empty()
                            && model.batches.empty() && model.waiters.empty()
                            && model.live == 0 && model.pending == 0,
                            "deep-chain scope retained records or operations");
                    const auto events = checked_events - events_before;
                    ++complete_scopes;
                    total_events += events;
                    total_records += model.next_branch;
                    peak_records = std::max(peak_records, scope_peak);
                    std::cout << "deep_chain_scope: shape=" << (side_branches ? "side_chain" : "chain")
                              << " fork_gate=" << (collecting_fork ? "Collecting" : "Open")
                              << " outcome=" << outcome_name << " actual_depth=" << actual_depth
                              << " peak_records=" << scope_peak << " created_records=" << model.next_branch
                              << " checked_events=" << events << " complete_scopes=1" << std::endl;
                } catch (const std::exception& error) {
                    throw std::runtime_error(std::string("deep_chain shape=")
                        + (side_branches ? "side_chain" : "chain") + " fork_gate="
                        + (collecting_fork ? "Collecting" : "Open") + " outcome=" + outcome_name
                        + " depth=" + std::to_string(depth) + " level=" + std::to_string(level)
                        + " stage=" + stage + ": " + error.what());
                }
            }
        }
    }
    std::cout << "deep_chain_model: requested_depth=" << depth << " actual_depth=" << depth
              << " peak_records=" << peak_records << " created_records=" << total_records
              << " checked_events=" << total_events << " complete_scopes=" << complete_scopes << '\n';
}

int positive_argument(const char* text) {
    std::size_t parsed = 0;
    const int result = std::stoi(text, &parsed);
    require(parsed == std::string(text).size() && result > 0, "expected positive integer");
    return result;
}

} // namespace

int main(int argc, char** argv) {
    try {
        int depth = 7, steps = 10000, seeds = 20, max_records = 16, chain_depth = 0;
        bool chain_only = false;
        for (int i = 1; i < argc; i += 2) {
            require(i + 1 < argc,
                    "usage: --depth N --random-steps N --seeds N --max-records N --chain-depth N --chain-only 1");
            const std::string flag = argv[i];
            const int value = positive_argument(argv[i + 1]);
            if (flag == "--depth") depth = value;
            else if (flag == "--random-steps") steps = value;
            else if (flag == "--seeds") seeds = value;
            else if (flag == "--max-records") max_records = value;
            else if (flag == "--chain-depth") {
                require(value <= 4096, "chain-depth exceeds bounded resource limit 4096");
                chain_depth = value;
            } else if (flag == "--chain-only") {
                require(value == 1, "chain-only must be 1");
                chain_only = true;
            }
            else throw std::runtime_error("unknown argument: " + flag);
        }
        require(!chain_only || chain_depth != 0, "chain-only requires --chain-depth N");
        if (!chain_only) {
            const int directed = directed_tests() + nested_directed_tests() + nested_overlap_tests()
                + nested_parent_precancel_tests();
            const int mutations = mutation_tests();
            std::cout << "directed_scenarios=" << directed << " mutations_rejected=" << mutations << '\n';
            explore(depth);
            random_runs(seeds, steps, max_records);
        } else {
            std::cout << "chain_mutations_rejected=" << mutation_tests() << '\n';
        }
        if (chain_depth != 0) deep_chain_tests(chain_depth);
        std::cout << "PASS (design model only; not production concurrency/lifetime proof)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
