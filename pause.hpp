#ifndef PAUSE_HPP_
#define PAUSE_HPP_

// 可选扩展：1 公开独立顶层分支登记，0 使用唯一 root 派生的默认接口。
// 包含本头文件的所有翻译单元必须使用同一值，否则违反单一定义规则（ODR）。
#ifndef PAUSE_ENABLE_INDEPENDENT_BRANCH
#define PAUSE_ENABLE_INDEPENDENT_BRANCH 0
#endif

// 协作式暂停库：只在业务显式等待 suspend_point() 时暂停，不抢占正在执行的工作。
// PauseSource 管理一组暂停责任；PauseBranch 拥有其中一条串行路径的责任记录。
// 线程、任务启动/组合、stop source、执行器进展及最终 join 均由宿主管理。
//
// 阅读顺序：公共值类型 -> detail 状态与完成协议 -> PauseBranch -> PauseSource。
// 详细契约见 pause-branch-design.md；接入与关闭顺序见 docs/user-guide.md。
// 当前验证基线是固定 stdexec 的默认 task/allocator 和 run_loop；环境 concept
// 只检查必要接口，不能证明任意 scheduler 后端、版本或递归深度都安全。
// 特别是深层 task/when_all 的业务异常传播仍可能耗尽宿主线程栈。
//
// 将不依赖 stdexec 的枚举置于重型 include 前，保留 clice 的 PCH 前缀规避布局。
namespace coop_pause {
// 恢复时 Continuous 打开门持续运行；SingleStep 先建立下一轮屏障，再放行当前 waiter。
// 单步单位是各分支到下一个安全点之间的工作，不是全局只执行一条业务指令。
enum class AdvanceMode { Continuous, SingleStep };
// 当前关闭门的来源；构造时选择 SingleStep，也以 SingleStep 为来源。
enum class PauseOrigin { ControlRequest, SingleStep };
// Open：可直接通过；Collecting：等待责任到达/撤销；Paused：本轮收集已完成。
enum class GatePhase { Open, Collecting, Paused };
}

#include <exec/completion_behavior.hpp>
#include <stdexec/execution.hpp>

#include <array>
#include <cassert>
#include <concepts>
#include <cstddef>
#include <exception>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace coop_pause {

struct PausePointInfo {
    // 不透明的业务标记；当前实现只携带此值，不据此调度、查询位置或输出日志。
    std::size_t tag = 0;
};

// 恢复凭证，取建立本轮 Paused 时的 revision。只能用于产生它的同一个 Source；
// 此类型不携带 Source 身份，跨 Source 混用不会被可靠检测。计数回绕不在保证范围内。
struct PauseEpoch {
    std::size_t value;
    friend bool operator==(PauseEpoch, PauseEpoch) = default;
};

enum class PauseError {
    RootNotCreated,         // 尚未成功创建 root，不能执行依赖初始化的操作。
    RootAlreadyCreated,     // root 已成功创建过；即使其 branch 已退役也不能再次创建。
    PauseAlreadyCollecting, // request_pause 时已在收集责任。
    AlreadyPaused,         // request_pause 时已暂停。
    NotPaused,             // resume 时不处于 Paused。
    StalePauseEpoch,        // resume 的凭证与当前 Paused 不匹配。
    InvalidBranchState,    // 当前门/branch 状态不允许创建独立责任或执行 fork。
    InvalidForkCount,      // fork 数量为零；不进行资源准备。
    ResourceExhausted      // 创建/fork 中可捕获的分配或容器长度失败，未提交状态。
};

// 同一次状态锁内取得的拥有型快照；它不是工作流完成信号或资源释放凭证。
struct PauseSnapshot {
    // 已提交的状态变化序号；通知可合并，观察到跳号不表示丢失状态一致性。
    std::size_t revision;
    // 只表示 root 曾成功创建，不表示根任务正在运行。
    bool root_created;
    GatePhase phase;
    // 仅 Paused 时有值；同轮 waiter 加入/撤销可改变 revision 而不改变 epoch。
    std::optional<PauseEpoch> paused_epoch;
    // Open 时为空；关闭门后记录控制请求或单步来源。
    std::optional<PauseOrigin> origin;
    // 初始 SingleStep 为 0，后续单步递增；恢复 Continuous 后清空。
    std::optional<std::size_t> step_round;
    // 仅计共享 Waiting 链中的节点，不含已摘链、尚在恢复调度队列中的 operation。
    std::size_t waiting_branch_count;
    // 当前参与屏障的责任数；不等于存活 record、task 或协程帧的数量。
    std::size_t live_branch_count;
};

// 可选的阶段通知；构造 Source 后不再修改。回调须线程安全、非阻塞，不同步重入 Pause。
// 只通知完整事务最终进入的阶段，不通知同阶段的计数/revision/epoch 变化。
// 锁外调用但可能并发、乱序；捕获对象须覆盖所有调用尾部，不能在通知中销毁 Source。
// 回调表示曾提交进入该阶段，不保证调用时仍处于该阶段；控制操作仍须校验状态/epoch。
struct PausePhaseNotifications {
    std::move_only_function<void() noexcept> on_open_changed;
    std::move_only_function<void() noexcept> on_collecting_changed;
    std::move_only_function<void() noexcept> on_paused_changed;
};

namespace detail {

// 无法继续的内部不变量/调用契约错误：Debug 提供断言，Release 仍然终止进程。
// 可恢复的控制校验与资源准备错误另外通过 expected 返回。
inline void require(bool condition) noexcept
{
    assert(condition);
    if (!condition) std::terminate();
}

inline std::lock_guard<std::mutex> lock_state(std::mutex& mutex) noexcept
{
    // 返回不可复制的 RAII guard，依赖保证的复制消除；锁覆盖调用方 guard 的作用域。
    // 系统 mutex 加锁失败不转成 PauseError，异常离开 noexcept 时终止进程。
    return std::lock_guard<std::mutex>{mutex};
}

template<class Node>
struct IntrusiveHook {
    // 节点内嵌链接；地址必须稳定，且同一 hook 同时只能属于一条链。
    Node* previous = nullptr;
    Node* next = nullptr;
};

// 非拥有型侵入双链表：只修改指针，不分配内存、不调用用户代码，也不自行加锁。
// 共享链由 Source mutex 保护；恢复批次是取得完成权的 resume 调用私有的链。
// erase 清空链接后，同一 waiter 可立即从共享链转入私有批次。
template<class Node>
class IntrusiveList {
    Node* first_ = nullptr;
    Node* last_ = nullptr;

public:
    IntrusiveList() = default;
    IntrusiveList(const IntrusiveList&) = delete;
    IntrusiveList& operator=(const IntrusiveList&) = delete;

    bool empty() const noexcept { return first_ == nullptr; }
    Node* front() const noexcept { return first_; }
    bool contains_link(const Node& node) const noexcept
    {
        // O(1) 的链接状态检查，依赖调用方保证节点属于预期的链；不是通用成员验证。
        // 尤其其他链的非孤立节点也可能返回 true，不能用它验证任意外部指针。
        return first_ == &node || node.previous != nullptr || node.next != nullptr;
    }
    void push_back(Node& node) noexcept
    {
        require(node.previous == nullptr && node.next == nullptr && first_ != &node);
        node.previous = last_;
        if (last_) last_->next = &node;
        else first_ = &node;
        last_ = &node;
    }
    void erase(Node& node) noexcept
    {
        // 前置条件：node 确实在本链中；清空 hook 也便于后续重新挂链。
        require(contains_link(node));
        if (node.previous) node.previous->next = node.next;
        else first_ = node.next;
        if (node.next) node.next->previous = node.previous;
        else last_ = node.previous;
        node.previous = nullptr;
        node.next = nullptr;
    }
    Node* pop_front() noexcept
    {
        auto* node = first_;
        if (node) erase(*node);
        return node;
    }
};

struct PauseWaiter;
// 由一个 PauseBranch 的 unique_ptr 独占拥有；字段在登记后由 Source mutex 保护。
// 分支对象可移动，但 record 地址不变，供父子关系及暂停 operation 稳定借用。
struct BranchRecord : IntrusiveHook<BranchRecord> {
    // true 才计入 live_branch_count；已委托 children 或接受取消后可保留 record 而不参与。
    bool participates = false;
    // 当前 Collecting 轮尚欠一次到达/撤销；与全局 pending_arrivals 一一对应。
    bool arrival_pending = false;
    // 仅在共享 Waiting 阶段非空；摘链不表示完整公开暂停 operation 已完成。
    PauseWaiter* waiter = nullptr;
    // 尚未析构退役的直接 child records 数，不是参与责任数，也不包含更深后代。
    std::size_t live_children = 0;
    // 非拥有型直接父指针；parent 必须晚于所有直接 children 退役。
    BranchRecord* parent = nullptr;
};

// 这是 raw operation 的完成权状态，不是 GatePhase，也不表示完整 sender 已完成。
// Registering -> CancelRequested：callback 构造期只记录取消，完成交给 start。
// Registering -> Waiting/CompletingValue；CancelRequested -> CompletingStopped。
// Waiting -> CompletingValue/CompletingStopped：resume/callback 在同一状态锁下竞争。
enum class OperationPhase {
    Registering, CancelRequested, Waiting, CompletingValue, CompletingStopped
};
// start 的局部裁决结果；裁决结束后 None 表示完成权已交给 Waiting 的后续竞争方。
enum class Completion { None, InlineValue, ResumedValue, Stopped };

// raw operation 内嵌的类型擦除节点；链表不拥有它，内存由具体 operation 管理。
struct PauseWaiter : IntrusiveHook<PauseWaiter> {
    BranchRecord* branch_record;
    OperationPhase phase = OperationPhase::Registering;
    // 完成权所有者在锁外注销 callback；可能等待另一线程的 callback 返回。
    void (*reset_callback)(PauseWaiter*) noexcept;
    // 交付 raw completion，接收方可能在调用中销毁当前节点。
    void (*deliver)(PauseWaiter*, Completion) noexcept;

    PauseWaiter(BranchRecord* branch, void (*reset)(PauseWaiter*) noexcept,
                void (*complete)(PauseWaiter*, Completion) noexcept) noexcept
        : branch_record(branch), reset_callback(reset), deliver(complete)
    {}
};

// Source 按值拥有的共享状态；所有已发布的可变记账字段均由 mutex 保护。
// 不在锁内构造/注销 callback、调度、完成 receiver、调用 notifier 或分配记录。
struct PauseStateData {
    mutable std::mutex mutex;
    // 借用 Source 自己拥有的阶段通知；线程安全由宿主保证。
    PausePhaseNotifications* notifications;
    bool root_created = false;
    GatePhase phase = GatePhase::Open;
    std::size_t revision = 0;
    std::optional<PauseEpoch> paused_epoch;
    std::optional<PauseOrigin> origin;
    std::optional<std::size_t> step_round;
    std::size_t live_branch_count = 0;
    // 仅 Collecting 时可非零；减到零后，由一次统一提交将门转为 Paused。
    std::size_t pending_arrivals = 0;
    // 包含所有尚未退役的记录，也包含委托父分支和已撤责但仍被 frame 持有的记录。
    IntrusiveList<BranchRecord> branches;
    // 仅含 phase == Waiting 的操作；已取得完成权的节点必须从这里摘走。
    IntrusiveList<PauseWaiter> waiters;

    explicit PauseStateData(PausePhaseNotifications& phase_notifications) noexcept
        : notifications(&phase_notifications) {}

    // 锁外分派本次提交记录的阶段，不重新读取可能已被其他事务修改的 phase。
    void notify_phase_changed(std::optional<GatePhase> entered_phase) noexcept
    {
        if (!entered_phase) return;
        switch (*entered_phase) {
        case GatePhase::Open:
            if (notifications->on_open_changed) notifications->on_open_changed();
            break;
        case GatePhase::Collecting:
            if (notifications->on_collecting_changed) notifications->on_collecting_changed();
            break;
        case GatePhase::Paused:
            if (notifications->on_paused_changed) notifications->on_paused_changed();
            break;
        }
    }
};

// 以下 *_locked 辅助函数都要求持有对应 Source 的 mutex。
// record 来源及实际生命期仍是调用方前置条件，链接检查不能识别悬空指针。
inline void require_registered_locked(const PauseStateData& state,
                                      const BranchRecord& branch) noexcept
{
    require(state.branches.contains_link(branch));
}

// Debug 在每次完整事务提交后重建责任、waiter 和父子计数；不分配、不调用外部代码。
// 按每个 record 扫描其 children，单次 O(R^2 + W)，并持有状态锁。
// 因此逐个退役 R 个记录累计可达 O(R^3)；Release 的 NDEBUG 构建不执行此检查。
inline void assert_invariants_locked(const PauseStateData& state) noexcept
{
#ifndef NDEBUG
    std::size_t live = 0, pending = 0, waiting = 0;
    for (auto* b = state.branches.front(); b; b = b->next) {
        live += b->participates;
        pending += b->arrival_pending;
        assert(!b->participates || b->live_children == 0);
        assert(b->participates || (!b->arrival_pending && b->waiter == nullptr));
        if (b->waiter) {
            ++waiting;
            assert(b->waiter->branch_record == b);
            assert(b->waiter->phase == OperationPhase::Waiting);
            assert(!b->arrival_pending && state.phase != GatePhase::Open);
        }
        // Paused 的参与者都已挂起；Collecting 的参与者恰为未到达或已等待二者之一。
        if (state.phase == GatePhase::Paused) assert(!b->participates || b->waiter);
        if (state.phase == GatePhase::Collecting && b->participates)
            assert(b->arrival_pending != (b->waiter != nullptr));
        std::size_t children = 0;
        for (auto* c = state.branches.front(); c; c = c->next)
            children += c->parent == b;
        assert(children == b->live_children);
        if (b->parent) assert(state.branches.contains_link(*b->parent));
    }
    std::size_t linked_waiting = 0;
    for (auto* w = state.waiters.front(); w; w = w->next) {
        ++linked_waiting;
        assert(w->branch_record->waiter == w);
        assert(w->phase == OperationPhase::Waiting);
    }
    assert(live == state.live_branch_count && pending == state.pending_arrivals);
    assert(waiting == linked_waiting);
    assert(state.phase == GatePhase::Collecting || pending == 0);
    assert(state.phase != GatePhase::Open || waiting == 0);
    assert((state.phase == GatePhase::Paused) == state.paused_epoch.has_value());
#else
    (void)state;
#endif
}

// 从 Open 建立一轮收集，当前参与者各欠一次到达；委托父分支等不参与记录不计入。
// 此函数不独立提交，允许调用者在同一事务内继续修改，再统一处理空屏障。
inline void begin_collecting_locked(PauseStateData& state, PauseOrigin origin) noexcept
{
    require(state.phase == GatePhase::Open);
    state.phase = GatePhase::Collecting;
    state.origin = origin;
    state.pending_arrivals = state.live_branch_count;
    for (auto* b = state.branches.front(); b; b = b->next)
        b->arrival_pending = b->participates;
}

// 同一责任在本轮至多扣减一次 pending；到达安全点和直接撤责共用此路径。
inline void satisfy_arrival_once_locked(PauseStateData& state, BranchRecord& branch) noexcept
{
    if (!branch.arrival_pending) return;
    require(state.phase == GatePhase::Collecting && state.pending_arrivals > 0);
    branch.arrival_pending = false;
    --state.pending_arrivals;
}

// 幂等撤销参与责任，但不摘除/销毁 record，也不修改 parent 的 live_children。
// 若有 waiter，调用者必须先取得完成权并摘链；返回值表示是否真的改变了记账。
inline bool withdraw_locked(PauseStateData& state, BranchRecord& branch) noexcept
{
    require(branch.waiter == nullptr);
    if (!branch.participates) return false;
    satisfy_arrival_once_locked(state, branch);
    branch.participates = false;
    require(state.live_branch_count > 0);
    --state.live_branch_count;
    return true;
}

// 最后一个直接 child 真正退役后交还 parent 责任；只记账，由外层事务统一提交。
inline void return_parent_responsibility_locked(PauseStateData& state,
                                               BranchRecord& parent) noexcept
{
    require_registered_locked(state, parent);
    require(parent.live_children == 0 && !parent.participates);
    require(!parent.arrival_pending && parent.waiter == nullptr);
    if (state.phase == GatePhase::Paused) {
        state.phase = GatePhase::Collecting;
        state.paused_epoch.reset();
        // 保留 origin、step_round 及已有 waiter，不重新标记其他参与者的到达责任。
    }
    parent.participates = true;
    ++state.live_branch_count;
    if (state.phase == GatePhase::Collecting) {
        parent.arrival_pending = true;
        ++state.pending_arrivals;
    }
}

// 每个完整事务提交一次；previous_phase 在首次修改共享状态前取得。
// 返回最终进入的阶段；同阶段的 revision/epoch 更新不产生阶段通知。
[[nodiscard]] inline std::optional<GatePhase> commit_change_locked(
    PauseStateData& state, GatePhase previous_phase) noexcept
{
    ++state.revision;
    if (state.phase == GatePhase::Collecting && state.pending_arrivals == 0) {
        state.phase = GatePhase::Paused;
        state.paused_epoch = PauseEpoch{state.revision};
    }
    assert_invariants_locked(state);
    if (state.phase == previous_phase) return std::nullopt;
    return state.phase;
}

// 公开 sender 的最终 stopped 记账：raw callback 可能已经撤责，也可能是恢复调度
// 才接受取消。后者必须补撤销 SingleStep 已建立的下一轮责任，不能等 frame 析构。
inline void withdraw_and_notify(PauseStateData* state, BranchRecord* branch) noexcept
{
    bool changed;
    std::optional<GatePhase> entered_phase;
    {
        auto lock = lock_state(state->mutex);
        require_registered_locked(*state, *branch);
        const auto previous_phase = state->phase;
        changed = withdraw_locked(*state, *branch);
        if (changed) entered_phase = commit_change_locked(*state, previous_phase);
    }
    if (changed) state->notify_phase_changed(entered_phase);
}

// Waiting 的唯一完成权裁决点。改 phase、摘共享链、清 branch.waiter 必须同锁完成。
// 失败者不得访问 receiver 或尝试注销 callback；成功者负责后续锁外交付。
inline bool claim_waiter_locked(PauseStateData& state, PauseWaiter& waiter,
                                OperationPhase target) noexcept
{
    if (waiter.phase != OperationPhase::Waiting) return false;
    require(waiter.branch_record->waiter == &waiter);
    waiter.phase = target;
    state.waiters.erase(waiter);
    waiter.branch_record->waiter = nullptr;
    return true;
}

using CompletionList = IntrusiveList<PauseWaiter>;

inline void complete_value_batch(CompletionList& batch) noexcept
{
    // 先注销整批 callback，再交付任何一个 receiver。首个 completion 可能同步
    // 请求兄弟取消、重入或触发析构，不能让批次后半段仍保留待注销的 callback。
    // 此时 resume 已取得整批完成权；callback 即使已被另一线程选中，也只能败退。
    for (auto* w = batch.front(); w; w = w->next) w->reset_callback(w);
    while (auto* w = batch.pop_front()) {
        w->deliver(w, Completion::ResumedValue);
        // terminal CPO 可能已销毁 w；必须先 pop，交付后不再读取节点或其链接。
    }
}

// 内部路由标签：只有等待后恢复才需要 schedule，公开 set_value() 不携带这些标签。
struct InlinePass {};
struct ResumedPass {};

// connect 的必要环境约束：原 inplace_stop_token 和启动 scheduler 都必须可查询。
// token 可以是空的 inplace_stop_token，但不能以 never_stop_token/std::stop_token 替代。
// concept 无法证明 start 确实运行在声明的 scheduler 上，也无法审查已擦除的后端。
template<class Env>
concept PauseEnvironment = requires(const Env& env) {
    { stdexec::get_start_scheduler(env) } -> stdexec::scheduler;
    requires std::same_as<std::remove_cvref_t<decltype(stdexec::get_stop_token(env))>,
                          stdexec::inplace_stop_token>;
};

// raw 层只处理挂起、完成权及取消撤责；恢复到执行器由外层 sender 组合负责。
// operation 必须单次 start，宿主不得通过提前析构来取消；receiver completion 内
// 的合法同步销毁则受支持。Source/record 必须覆盖完整公开 operation 的使用期。
template<class Receiver>
struct RawSuspendOperation : PauseWaiter {
    using operation_state_concept = stdexec::operation_state_tag;

    struct StopFn {
        RawSuspendOperation* operation;
        // on_stop 可以完成并销毁 operation；回到这里后不能再解引用该指针。
        void operator()() noexcept { operation->on_stop(); }
    };

    PauseStateData* state;
    PausePointInfo info;
    Receiver receiver;
    // 从已经就位的 receiver 环境获取本次原 token，不创建 Pause 专属停止源。
    // 宿主的 stop source 须覆盖 token/callback 的使用及尚未返回的 request_stop 调用栈。
    stdexec::inplace_stop_token token;
    // 最后声明，故析构时最先注销，先于 callback 可能访问的其他成员销毁。
    // optional 的构造/reset 权由本协议控制；它本身不支持任意并发 emplace/reset。
    std::optional<stdexec::inplace_stop_callback<StopFn>> callback;

    RawSuspendOperation(PauseStateData* source, BranchRecord* branch,
                        PausePointInfo point, Receiver target)
        : PauseWaiter(branch, &reset, &complete), state(source), info(point),
          receiver(std::move(target)), token(stdexec::get_stop_token(stdexec::get_env(receiver)))
    {}
    RawSuspendOperation(const RawSuspendOperation&) = delete;
    RawSuspendOperation& operator=(const RawSuspendOperation&) = delete;

    static void reset(PauseWaiter* waiter) noexcept
    {
        // 仅由完成权所有者在状态锁外调用。异线程注销可能等待 callback，
        // 若持有 Source mutex 等待，而 callback 正等该锁，就会形成死锁。
        static_cast<RawSuspendOperation*>(waiter)->callback.reset();
    }
    static void complete(PauseWaiter* waiter, Completion result) noexcept
    {
        // 每个 terminal CPO 都可能同步释放 self；交付后直接返回，不做尾部访问。
        auto* self = static_cast<RawSuspendOperation*>(waiter);
        switch (result) {
        case Completion::InlineValue:
            stdexec::set_value(std::move(self->receiver), InlinePass{});
            return;
        case Completion::ResumedValue:
            stdexec::set_value(std::move(self->receiver), ResumedPass{});
            return;
        case Completion::Stopped:
            stdexec::set_stopped(std::move(self->receiver));
            return;
        default:
            std::terminate();
        }
    }
    void start() & noexcept
    {
        // Waiting 发布后 this 可能在另一线程被完成并销毁，先保存通知尾部所需指针。
        // 这些局部指针只避免再次访问 this，并不延长 Source 或 record 的生命期。
        auto* const s = state;
        auto* const b = branch_record;
        Completion own = Completion::None;
        bool changed = false;
        std::optional<GatePhase> entered_phase;

        // 必须锁外注册：预停止可在 emplace 内同步回调，另一线程也可在注册刚可见时
        // 调用 on_stop。锁内仍见 Registering 时只记录取消，不能销毁尚在构造的 callback。
        // 异线程回调若直到 start 推进状态后才取得锁，则按它实际观察到的 phase 裁决。
        callback.emplace(token, StopFn{this});
        {
            auto lock = lock_state(s->mutex);
            require_registered_locked(*s, *b);
            const auto previous_phase = s->phase;
            // 同一 branch 不允许并行暂停，委托父分支也须等直接 children 全部退役。
            require(b->waiter == nullptr && b->live_children == 0);
            if (phase == OperationPhase::CancelRequested) {
                // callback 构造已经返回；start 取得 stopped 的清理/交付权。
                // 已撤责分支再次接受取消时，withdraw 是无变化的幂等操作。
                phase = OperationPhase::CompletingStopped;
                own = Completion::Stopped;
                changed = withdraw_locked(*s, *b);
            } else {
                require(phase == OperationPhase::Registering);
                const bool was_participating = b->participates;
                if (!was_participating) {
                    // 已撤责的有效 branch 再使用时恢复参与，仍须满足串行使用契约。
                    // 正常父子交接已在最后 child 退役时完成，不会在这里重复计数。
                    b->participates = true;
                    ++s->live_branch_count;
                    changed = true;
                }
                if (s->phase == GatePhase::Open) {
                    // 锁内选定直接通过；后来的 pause/stop 不会改写这次 raw 完成权。
                    // 原本已参与的普通 Open 快路径不改共享状态，也不增加 revision。
                    phase = OperationPhase::CompletingValue;
                    own = Completion::InlineValue;
                } else {
                    // Paused 中原参与者都已有 waiter，所以此时只能是非参与者重入。
                    // 重入者在同一事务内参与并到达，不新增一份未满足的 pending。
                    if (s->phase == GatePhase::Paused) require(!was_participating);
                    else if (was_participating) require(b->arrival_pending);
                    b->waiter = this;
                    s->waiters.push_back(*this);
                    satisfy_arrival_once_locked(*s, *b);
                    phase = OperationPhase::Waiting;
                    changed = true;
                }
            }
            if (changed) entered_phase = commit_change_locked(*s, previous_phase);
        }
        // 解锁即发布 Waiting：resume/callback 可在本 start 返回前完成并销毁 operation。
        // 未持有完成权时只用局部变量完成通知；即使通知晚到，观察方仍应读取最新快照。
        if (changed) s->notify_phase_changed(entered_phase);
        if (own == Completion::None) return;
        // start 自己拥有直接 value 或构造期 stopped，才可锁外注销并交付。
        callback.reset();
        complete(this, own);
    }
    void on_stop() noexcept
    {
        auto* const s = state;
        std::optional<GatePhase> entered_phase;
        {
            auto lock = lock_state(s->mutex);
            const auto previous_phase = s->phase;
            if (phase == OperationPhase::Registering) {
                // 不访问 optional，不提交 receiver；交给构造返回后的 start 处理。
                phase = OperationPhase::CancelRequested;
                return;
            }
            // 只争夺 Waiting；若 start/resume 已拥有完成权，立即退出，不改判、不 reset。
            if (!claim_waiter_locked(*s, *this, OperationPhase::CompletingStopped)) return;
            withdraw_locked(*s, *branch_record);
            entered_phase = commit_change_locked(*s, previous_phase);
        }
        s->notify_phase_changed(entered_phase);
        // callback 获胜时不显式 reset 自己；completion 中或之后的合法析构负责注销。
        stdexec::set_stopped(std::move(receiver));
        // 包含当前回调线程的自注销情况：此后不得再访问 this/receiver/record/callback。
    }
};

// 惰性的 raw sender：构造/connect 都不登记 waiter；实际裁决只发生在 start。
// 两种 value 标签和 stopped 是 raw 完成集合，外层组合可能额外贡献 error 签名。
struct RawSuspendSender {
    using sender_concept = stdexec::sender_tag;
    PauseStateData* state;
    BranchRecord* branch;
    PausePointInfo info;

    template<class, class... Env>
    static consteval auto get_completion_signatures()
    {
        return stdexec::completion_signatures<
            stdexec::set_value_t(InlinePass), stdexec::set_value_t(ResumedPass),
            stdexec::set_stopped_t()>{};
    }

    template<class Receiver>
        requires PauseEnvironment<stdexec::env_of_t<Receiver>>
    auto connect(Receiver receiver) const
    {
        return RawSuspendOperation<Receiver>{state, branch, info, std::move(receiver)};
    }
};

// 复用一层策略包装的两种角色：
// Account=true：公开暂停本体，最终 stopped 时幂等撤责，并声明本体的 value 行为。
// Account=false：.affine() 返回的宿主外层，责任已由本体处理，不再次记账。
// 两者都终止启动后到达本层的内部 error，不把它交给业务 catch/let_error。
template<class Inner, bool Account>
struct CompletionPolicySender {
    using sender_concept = stdexec::sender_tag;
    PauseStateData* state;
    BranchRecord* branch;
    Inner inner;

    struct Attrs {
        stdexec::env_of_t<Inner> inner;
        // 仅为能查询启动 scheduler 的环境声明 value：Open 内联，等待后恢复为
        // asynchronous_affine。此声明以宿主实际启动上下文符合其环境为前提。
        // raw stopped 可在取消线程交付，因此这里不宣称 stopped 也有执行器亲和性。
        template<class Env>
            requires Account && requires(const Env& env) {
                { stdexec::get_start_scheduler(env) } -> stdexec::scheduler;
            }
        static constexpr auto query(exec::get_completion_behavior_t<stdexec::set_value_t>,
                                    const Env&) noexcept
        {
            return exec::completion_behavior::inline_completion
                 | exec::completion_behavior::asynchronous_affine;
        }
        // value 的 inline/恢复两条路径均属于启动执行资源；原生线程池有自己的 domain。
        // 不能转发 raw 组合的保守 domain，同时又让 affinity 推导出线程池完成 scheduler。
        template<class Env>
            requires Account && requires(const Env& env) {
                { stdexec::get_start_scheduler(env) } -> stdexec::scheduler;
            }
        static constexpr auto query(stdexec::get_completion_domain_t<stdexec::set_value_t>,
                                    const Env& env) noexcept
        {
            return stdexec::get_domain(env);
        }
        template<class Query, class... Args>
            requires (stdexec::forwarding_query(Query{})
                      && !(Account && std::same_as<Query,
                          exec::get_completion_behavior_t<stdexec::set_value_t>>)
                      && !(Account && std::same_as<Query,
                          stdexec::get_completion_domain_t<stdexec::set_value_t>>)
                      && requires(const stdexec::env_of_t<Inner>& env,
                                  Query query, const Args&... args) { query(env, args...); })
        decltype(auto) query(Query query, const Args&... args) const
            noexcept(noexcept(query(inner, args...)))
        {
            // 其余合法 forwarding query 沿用 inner；不伪造完成 scheduler。
            // 本体不转发 inner 的 value 行为属性；由上面的带环境定制或查询 CPO 的保守回退决定。
            return query(inner, args...);
        }
    };
    auto get_env() const noexcept { return Attrs{stdexec::get_env(inner)}; }

    template<class, class... Env>
    static consteval auto get_completion_signatures()
    {
        // 保留 Inner 的完整签名（含 error），不把运行时 terminate 策略伪装成
        // 编译期绝无 error。公开 receiver 实际只收到 value/stopped。
        return stdexec::get_completion_signatures<Inner, Env...>();
    }

    template<class Receiver>
    struct PolicyReceiver {
        using receiver_concept = stdexec::receiver_tag;
        PauseStateData* state;
        BranchRecord* branch;
        Receiver receiver;
        // 原样保留 receiver environment，尤其是本次组合器提供的 stop token。
        // 例如 when_all 的组内 token 是这里的原 token，不应绕过它寻找根 token。
        decltype(auto) get_env() const noexcept { return stdexec::get_env(receiver); }
        void set_value() noexcept { stdexec::set_value(std::move(receiver)); }
        void set_stopped() noexcept
        {
            // 先清理责任再向外完成；外层可能恢复协程、排入宿主跳转或销毁当前对象。
            if constexpr (Account) withdraw_and_notify(state, branch);
            stdexec::set_stopped(std::move(receiver));
        }
        // 内部基础设施失败是明确的进程终止策略，不是业务 error 的统一处理方式。
        // 不承诺在 terminate 前补做撤责、通知、析构或排空恢复批次。
        template<class Error>
        [[noreturn]] void set_error(Error&&) noexcept { std::terminate(); }
    };

    template<class Receiver>
        requires PauseEnvironment<stdexec::env_of_t<Receiver>>
    auto connect(Receiver receiver) &&
    {
        // 由组合器返回的 operation 拥有完整内层操作，无需额外手写 start 转发层。
        return stdexec::connect(std::move(inner),
            PolicyReceiver<Receiver>{state, branch, std::move(receiver)});
    }

    // 固定默认 stdexec::task 直接 co_await 本体时选用此成员定制。
    // 正常 value 已满足 0/1 次本体调度，不再加跳转；只为 stopped 增加一次宿主跳转。
    // 普通 connect/start 未请求 affine 时，没有这一层，stopped 仍可在取消线程交付。
    auto affine() && requires Account
    {
        auto adapted = stdexec::let_stopped(std::move(*this), [] {
            // 进入此处前，本体已经撤责并通知；之后的跳转不重新加入屏障。
            return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
                [](auto scheduler) {
                    return stdexec::let_value(
                        // 只屏蔽这个“交付既定 stopped”的跳转的取消，不屏蔽正常恢复调度。
                        // 宿主须保证核准后端在此不可取消路径不会 error/stopped，并持续驱动队列。
                        stdexec::unstoppable(stdexec::schedule(scheduler)),
                        [] { return stdexec::just_stopped(); });
                });
        });
        // 新增的宿主组合也要有最外层 error 终止包装；false 不会解引用这两个空指针。
        return CompletionPolicySender<decltype(adapted), false>{nullptr, nullptr, std::move(adapted)};
    }
};

} // namespace detail

class PauseSource;

// 一条串行业务路径的暂停责任所有者。Source 必须活得更久；不同并发路径各持一份 branch。
// 同一 branch 的暂停、fork、移动、析构必须串行，且覆盖完整公开 operation/宿主适配层。
// Branch 不拥有任务或 stop source，不会在析构时请求取消或等待任务结束。
class PauseBranch {
public:
    PauseBranch(const PauseBranch&) = delete;
    PauseBranch& operator=(const PauseBranch&) = delete;
    // 仅转移所有权，不改变 record 地址或共享计数；被移出的对象析构时不再退役责任。
    PauseBranch(PauseBranch&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)), record_(std::move(other.record_))
    {}
    PauseBranch& operator=(PauseBranch&&) = delete;
    // RAII 退役：要求无活跃暂停操作、所有直接 child records 已释放。
    // 锁内检查只能捕获部分违规；恢复调度期间 waiter 已空，并不意味着可以提前析构。
    ~PauseBranch() noexcept
    {
        // 覆盖 moved-from，以及资源准备失败/提交被拒绝时尚未登记的临时对象。
        if (!state_ || !record_) return;
        auto* const s = state_;
        std::optional<GatePhase> entered_phase;
        {
            auto lock = detail::lock_state(s->mutex);
            const auto previous_phase = s->phase;
            auto& b = *record_;
            detail::require_registered_locked(*s, b);
            detail::require(b.waiter == nullptr && b.live_children == 0);
            detail::withdraw_locked(*s, b);
            s->branches.erase(b);
            if (auto* const parent = b.parent) {
                // 只有 record 真正退役才减少直接子数；取消撤责不会提前触发这里。
                detail::require(parent->live_children > 0);
                --parent->live_children;
                b.parent = nullptr;
                if (parent->live_children == 0)
                    detail::return_parent_responsibility_locked(*s, *parent);
            }
            // child 退出与 parent 接回完成后再统一收敛，避免发布中间的空 Paused。
            entered_phase = detail::commit_change_locked(*s, previous_phase);
        }
        s->notify_phase_changed(entered_phase);
        // record_ 在函数体之后于锁外释放存储；Source 须覆盖这段通知/析构尾部。
    }

    // 构造惰性 sender，构造本身不表示已到达安全点，也不预先决定当前门是否关闭。
    // root、child、独立分支的第一段受保护业务前都要真正 await/start 一次入口暂停点。
    // 返回值借用 Source/record，应在 branch 的结构化生命周期中立即等待，不可 detach。
    // sender 构造/最外层 connect 可按普通 C++ 规则抛异常；启动后内部组合/调度的
    // error 由策略层直接 terminate，不交给公开 receiver 的业务 error 处理器。
    // 普通外部 sender 的 stopped/error 不会经过本体撤责，宿主必须及时释放其 branch；
    // 吞掉 stopped 后延迟 frame 释放可能阻止收敛，见设计契约的 stopped_as_optional 包装。
    [[nodiscard]] auto suspend_point(PausePointInfo info = {}) &
    {
        auto* s = state_;
        auto* b = record_.get();
        detail::require(s != nullptr && b != nullptr);
        // start 时由 raw 层处理取消，并在状态锁内裁决直接通过或登记等待。
        auto routed = stdexec::let_value(detail::RawSuspendSender{s, b, info},
            [](auto route) {
                if constexpr (std::same_as<decltype(route), detail::InlinePass>) {
                    // 原本就在环境声明的启动上下文：不读取 scheduler，不额外 schedule，
                    // 也不隐式 yield，避免 Open 路径复制类型擦除 scheduler 的成本。
                    return stdexec::just();
                } else {
                    static_assert(std::same_as<decltype(route), detail::ResumedPass>);
                    // 已登记 Waiting 的恢复始终走此路，即使 resume 早于 raw start 返回。
                    // 从 receiver 环境读取启动 scheduler，不依赖 resume 调用线程的资源。
                    return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
                        [](auto scheduler) {
                            // 保留原 token；若排队后接受取消，外层策略会撤销下一轮责任。
                            return stdexec::schedule(scheduler);
                        });
                }
            });
        return detail::CompletionPolicySender<decltype(routed), true>{s, b, std::move(routed)};
    }
    // 禁止从临时 branch 建立可能悬空的借用；Source 工厂/控制 API 也只接受左值。
    auto suspend_point(PausePointInfo = {}) && = delete;

    // 将父责任原子转交给 N 个直接 children，返回可移动的拥有型数组。
    // block_self 指父分支暂不参与屏障/不执行业务，不表示阻塞当前线程或自动启动 children。
    // 父分支必须有效、参与、无 waiter/存活直接 children，且门为 Open 或 Collecting。
    // child 全部退役后 parent 自动接回责任，可直接再次 fork；继续受保护业务前须经过暂停点。
    // N==0 优先返回 InvalidForkCount；其余情况先准备资源，故资源错误可早于状态错误。
    template<std::size_t N>
    [[nodiscard]] std::expected<std::array<PauseBranch, N>, PauseError>
    fork_and_block_self() &
    {
        if constexpr (N == 0) {
            return std::unexpected(PauseError::InvalidForkCount);
        } else {
            try {
                // 聚合构造避免要求 PauseBranch 默认构造/赋值；准备对象尚未登记，
                // 任一分配失败时已构造的 unique_ptr 自动回收，不修改父责任。
                auto children = []<std::size_t... Index>(std::index_sequence<Index...>) {
                    return std::array<PauseBranch, N>{((void)Index, prepare())...};
                }(std::make_index_sequence<N>{});
                return commit_prepared_fork(std::move(children));
            } catch (const std::bad_alloc&) {
                return std::unexpected(PauseError::ResourceExhausted);
            }
        }
    }

    // 动态宽度版本，责任协议与固定宽度相同；先 reserve 再准备所有 records。
    // 可捕获的 bad_alloc/length_error 返回 ResourceExhausted。MSVC 默认 Debug STL
    // 的 vector 在 noexcept 构造/移动内可能分配代理，该分配失败会直接 terminate，
    // 不能被这里的 catch 转译；noexcept 返回移动不代表标准库内部一定不分配。
    [[nodiscard]] std::expected<std::vector<PauseBranch>, PauseError>
    fork_and_block_self(std::size_t count) &
    {
        if (count == 0) return std::unexpected(PauseError::InvalidForkCount);
        try {
            std::vector<PauseBranch> children;
            children.reserve(count);
            for (std::size_t i = 0; i != count; ++i) children.push_back(prepare());
            return commit_prepared_fork(std::move(children));
        } catch (const std::bad_alloc&) {
            return std::unexpected(PauseError::ResourceExhausted);
        } catch (const std::length_error&) {
            return std::unexpected(PauseError::ResourceExhausted);
        }
    }

private:
    friend class PauseSource;
    // 非拥有型 Source 借用；nullptr 表示 moved-from 或尚未提交的内部准备对象。
    detail::PauseStateData* state_;
    // 独占稳定地址的 record；移入任务不会改变父子指针和 waiter 借用的地址。
    std::unique_ptr<detail::BranchRecord> record_;

    PauseBranch(detail::PauseStateData* state,
                std::unique_ptr<detail::BranchRecord> record) noexcept
        : state_(state), record_(std::move(record))
    {}
    // 锁外资源准备，不登记责任；由 make_branch_impl/commit_prepared_fork 统一提交。
    static PauseBranch prepare()
    {
        return {nullptr, std::make_unique<detail::BranchRecord>()};
    }
    // children 入参拥有所有准备记录。校验失败时它们自动回收；校验通过后在同一锁内
    // 完成“撤父、登记全部子、提交”，中途不得发布因暂时零责任产生的虚假 Paused。
    template<class Container>
    std::expected<Container, PauseError> commit_prepared_fork(Container children) & noexcept
    {
        static_assert(std::is_nothrow_move_constructible_v<Container>);
        auto* const s = state_;
        if (!s || !record_) return std::unexpected(PauseError::InvalidBranchState);
        std::optional<GatePhase> entered_phase;
        {
            auto lock = detail::lock_state(s->mutex);
            const auto previous_phase = s->phase;
            auto& parent = *record_;
            detail::require_registered_locked(*s, parent);
            if (s->phase == GatePhase::Paused || !parent.participates ||
                parent.waiter != nullptr || parent.live_children != 0)
                return std::unexpected(PauseError::InvalidBranchState);
            if (s->phase == GatePhase::Collecting) detail::require(parent.arrival_pending);

            detail::withdraw_locked(*s, parent);
            parent.live_children = children.size();
            for (auto& child : children) {
                auto& b = *child.record_;
                b.parent = &parent;
                b.participates = true;
                // Collecting 中原父欠一次到达，转交后改由每个 child 各欠一次。
                b.arrival_pending = s->phase == GatePhase::Collecting;
                child.state_ = s;
                s->branches.push_back(b);
                ++s->live_branch_count;
                if (b.arrival_pending) ++s->pending_arrivals;
            }
            entered_phase = detail::commit_change_locked(*s, previous_phase);
        }
        s->notify_phase_changed(entered_phase);
        // 共享状态已完整提交；仅移动已有所有权，不再准备 Pause 自己的记录资源。
        return std::expected<Container, PauseError>{std::in_place, std::move(children)};
    }
};

// 一次完整工作流执行中，各 branch 共享的暂停门；执行结束后销毁，不复用或重置。
// root 按普通 branch 使用，持有者由宿主安排；所有 branch 均借用本 Source。
// 地址稳定且不可复制/移动；控制/创建/快照调用通过 mutex
// 串行化共享事务，但阶段通知可并发。Source 不拥有借用它的 branch、operation 或线程。
// 关闭时宿主必须释放全部 branch/operation，并等待控制、观察、callback、raw start
// 及通知调用尾部退出，再销毁 Source；根 completion 或计数归零均不足以替代这些条件。
class PauseSource {
public:
    // 构造后门为 Open；初始 SingleStep 到 root 成功登记时才建立第 0 轮屏障。
    // 阶段通知可全部为空；捕获对象须遵守上面的宿主生命周期约束。
    PauseSource(AdvanceMode initial_mode, PausePhaseNotifications notifications = {})
        : initial_mode_(initial_mode), notifications_(std::move(notifications)),
          state_(notifications_)
    {}
    // 不取消、不完成 waiter、不等待外部调用；销毁前的静默状态由宿主保证。
    ~PauseSource() noexcept
    {
        // Debug 只检查可见记录/计数；无法证明锁外调用尾部已退出，Release 也不保留这些断言。
        // 无 root、Open、空 Paused 均可作为合法终态，不要求固定的最终门状态。
        assert(state_.branches.empty() && state_.waiters.empty());
        assert(state_.live_branch_count == 0 && state_.pending_arrivals == 0);
    }
    PauseSource(const PauseSource&) = delete;
    PauseSource& operator=(const PauseSource&) = delete;
    PauseSource(PauseSource&&) = delete;
    PauseSource& operator=(PauseSource&&) = delete;

    // 一生仅成功一次；创建即登记责任但不启动任务。初始 SingleStep 在同一事务中关门，
    // 所以调用方先等待入口暂停点，就不会在首次放行前执行受保护业务。
    // 锁内重复创建返回 RootAlreadyCreated；此前的准备分配仍可能先失败。
    [[nodiscard]] std::expected<PauseBranch, PauseError> make_root_branch() &
    {
        return make_branch_impl(true);
    }
    std::expected<PauseBranch, PauseError> make_root_branch() && = delete;

#if PAUSE_ENABLE_INDEPENDENT_BRANCH
    // 可选扩展：创建无 parent 的独立顶层责任；要求 root 曾创建且门为 Open。
    // 锁内先检查 RootNotCreated，再检查 InvalidBranchState；工作流内部展开用 fork。
    [[nodiscard]] std::expected<PauseBranch, PauseError> make_branch() &
    {
        return make_branch_impl(false);
    }
    std::expected<PauseBranch, PauseError> make_branch() && = delete;
#endif

    // 关闭门并开始收集，不抢占业务，也不等待所有分支到达；成功不等于已经 Paused。
    // 责任已为零时同次提交可形成空 Paused，仍不能将其当成工作流完成。
    // 错误顺序：RootNotCreated -> PauseAlreadyCollecting -> AlreadyPaused。
    [[nodiscard]] std::expected<void, PauseError> request_pause() &
    {
        auto* const s = &state_;
        std::optional<GatePhase> entered_phase;
        {
            auto lock = detail::lock_state(s->mutex);
            const auto previous_phase = s->phase;
            if (!s->root_created) return std::unexpected(PauseError::RootNotCreated);
            if (s->phase == GatePhase::Collecting)
                return std::unexpected(PauseError::PauseAlreadyCollecting);
            if (s->phase == GatePhase::Paused) return std::unexpected(PauseError::AlreadyPaused);
            detail::begin_collecting_locked(*s, PauseOrigin::ControlRequest);
            entered_phase = detail::commit_change_locked(*s, previous_phase);
        }
        s->notify_phase_changed(entered_phase);
        return {};
    }
    std::expected<void, PauseError> request_pause() && = delete;

    // 调用方须提供同一 Source 当前的 paused_epoch；锁内只校验状态/数值，不验证来源身份。
    // 校验通过后，一次性取得全部 Waiting 的完成权。
    // Continuous 开门；SingleStep 先建立下一轮，再交付本批恢复，防止分支漏记下一轮责任。
    // 错误顺序：RootNotCreated -> NotPaused -> StalePauseEpoch；失败不改共享状态。
    // 成功仅表示本批 raw 恢复已交付，不等待公开 operation 完成或业务执行到下一安全点。
    // callback 注销可能等待其他线程，因此此 API 不是无等待/硬实时操作。
    [[nodiscard]] std::expected<void, PauseError> resume(PauseEpoch epoch, AdvanceMode mode) &
    {
        auto* const s = &state_;
        detail::CompletionList batch;
        std::optional<GatePhase> entered_phase;
        {
            auto lock = detail::lock_state(s->mutex);
            const auto previous_phase = s->phase;
            if (!s->root_created) return std::unexpected(PauseError::RootNotCreated);
            if (s->phase != GatePhase::Paused) return std::unexpected(PauseError::NotPaused);
            if (s->paused_epoch != epoch) return std::unexpected(PauseError::StalePauseEpoch);
            // 同锁摘入当前调用的私有批次；取消线程此后无法再取得这些 raw waiter 的完成权。
            while (auto* w = s->waiters.front()) {
                const bool won = detail::claim_waiter_locked(
                    *s, *w, detail::OperationPhase::CompletingValue);
                detail::require(won);
                batch.push_back(*w);
            }
            s->paused_epoch.reset();
            s->phase = GatePhase::Open;
            if (mode == AdvanceMode::Continuous) {
                s->origin.reset();
                s->step_round.reset();
            } else {
                // 原 waiter 仍是有效参与责任；先让它们欠下一次到达，再锁外交付恢复。
                // 如果本批为空且没有有效责任，commit 会立即生成新一轮空 Paused/epoch。
                s->step_round = s->step_round.value_or(0) + 1;
                detail::begin_collecting_locked(*s, PauseOrigin::SingleStep);
            }
            entered_phase = detail::commit_change_locked(*s, previous_phase);
        }
        s->notify_phase_changed(entered_phase);
        // 通知与 batch 清理均在锁外。恢复 schedule 仍保留取消响应：raw value 获胜
        // 并不保证最终公开 value，调度接受 stop 时由本体策略补撤责并交付 stopped。
        detail::complete_value_batch(batch);
        return {};
    }
    std::expected<void, PauseError> resume(PauseEpoch, AdvanceMode) && = delete;

    // 便利入口：尝试恢复本次调用观察到的暂停轮次，不绑定调用前的业务命令。
    // 仅 O(1) 读取必要状态，不遍历 snapshot 的 waiter 链；解锁后复用双参数校验。
    // 两次加锁之间若发生竞争，原样返回 NotPaused/StalePauseEpoch，不自动重试。
    [[nodiscard]] std::expected<void, PauseError> resume(AdvanceMode mode) &
    {
        const auto epoch = [&]() -> std::expected<PauseEpoch, PauseError> {
            auto lock = detail::lock_state(state_.mutex);
            if (!state_.root_created) return std::unexpected(PauseError::RootNotCreated);
            if (state_.phase != GatePhase::Paused) return std::unexpected(PauseError::NotPaused);
            return *state_.paused_epoch;
        }();
        if (!epoch) return std::unexpected(epoch.error());
        return resume(*epoch, mode);
    }
    std::expected<void, PauseError> resume(AdvanceMode) && = delete;

    // 在同一锁内复制一致快照；等待数现算，复杂度 O(W)，不是常数时间的原子读取。
    // 返回后状态可立即改变，控制方提交 resume 时仍须让 Source 重新校验 epoch。
    PauseSnapshot snapshot() const & noexcept
    {
        auto lock = detail::lock_state(state_.mutex);
        std::size_t waiting = 0;
        for (auto* w = state_.waiters.front(); w; w = w->next) ++waiting;
        return {state_.revision, state_.root_created, state_.phase, state_.paused_epoch,
                state_.origin, state_.step_round, waiting, state_.live_branch_count};
    }
    PauseSnapshot snapshot() const && noexcept = delete;

private:
    // 声明顺序保证 notifications_ 早于 state_ 构造、晚于 state_ 析构。
    // initial_mode_ 只控制首次 root 登记，不随后续 resume 的模式改变。
    const AdvanceMode initial_mode_;
    PausePhaseNotifications notifications_;
    detail::PauseStateData state_;

    // 与 fork 相同，先在锁外准备内存再校验并发状态；不承诺资源不足时优先返回状态错误。
    // 失败时准备对象的 state_ 为空，析构只释放私有存储，不提交 revision 或通知。
    std::expected<PauseBranch, PauseError> make_branch_impl(bool root)
    {
        try {
            auto branch = PauseBranch::prepare();
            auto* const s = &state_;
            std::optional<GatePhase> entered_phase;
            {
                auto lock = detail::lock_state(s->mutex);
                const auto previous_phase = s->phase;
                if (root) {
                    if (s->root_created) return std::unexpected(PauseError::RootAlreadyCreated);
                    s->root_created = true;
                } else {
                    if (!s->root_created) return std::unexpected(PauseError::RootNotCreated);
                    if (s->phase != GatePhase::Open)
                        return std::unexpected(PauseError::InvalidBranchState);
                }
                // 从这里开始正式登记；root_created、责任和可能的初始关门在同锁内提交。
                branch.state_ = s;
                branch.record_->participates = true;
                s->branches.push_back(*branch.record_);
                ++s->live_branch_count;
                if (root && initial_mode_ == AdvanceMode::SingleStep) {
                    s->step_round = 0;
                    detail::begin_collecting_locked(*s, PauseOrigin::SingleStep);
                }
                entered_phase = detail::commit_change_locked(*s, previous_phase);
            }
            s->notify_phase_changed(entered_phase);
            // 返回前责任已经生效；宿主必须最终启动拥有此 branch 的任务或释放它。
            return std::expected<PauseBranch, PauseError>{std::in_place, std::move(branch)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PauseError::ResourceExhausted);
        }
    }
};

} // namespace coop_pause

#endif  // PAUSE_HPP_
