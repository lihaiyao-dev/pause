// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// Copyright (c) 2026 Pause contributors.

/// @file pause.hpp
/// @brief Cooperative pause gates for stdexec workflows.
///
/// Paths pause only at started suspend_point() operations. The host controls task
/// startup, stop requests, scheduler progress, and joining. Keep stop sources alive
/// until their callbacks and request_stop() calls finish. Destroy PauseSource only
/// after all branches, operations, and calls using it have finished.
///
/// Receiver environments must supply a starting scheduler and stdexec::inplace_stop_token;
/// the host must start the operation on the declared resource. Internal error
/// completions after start terminate the process. Deep task/when_all exception
/// propagation may still exhaust the thread stack.
///
/// See docs/user-guide.md for integration and shutdown, and docs/design.md for
/// the complete contract and supported scheduler/host configurations.

#ifndef PAUSE_HPP_
#define PAUSE_HPP_

// All translation units must use the same value for this optional extension (ODR).
#ifndef PAUSE_ENABLE_INDEPENDENT_BRANCH
#define PAUSE_ENABLE_INDEPENDENT_BRANCH 0
#endif

// Keep these enums before the stdexec includes to bound the precompiled-header prefix.
namespace coop_pause {

/// Continuous opens the gate; SingleStep arms the next barrier before releasing waiters.
enum class AdvanceMode { Continuous, SingleStep };
enum class PauseOrigin { ControlRequest, SingleStep };

/// Open permits passage; Collecting awaits arrivals; Paused has no outstanding arrivals.
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
    std::size_t tag = 0; ///< Opaque application tag; not used for scheduling.
};

/// A pause-round token for its originating PauseSource. Source identity is not checked.
/// Counter wraparound is outside the supported contract.
struct PauseEpoch {
    std::size_t value;
    friend bool operator==(PauseEpoch, PauseEpoch) = default;
};

/// Recoverable errors reported through std::expected.
enum class PauseError {
    RootNotCreated,
    RootAlreadyCreated,
    PauseAlreadyCollecting,
    AlreadyPaused,
    NotPaused,
    StalePauseEpoch,
    InvalidBranchState,
    InvalidForkCount,
    ResourceExhausted
};

/// A consistent state snapshot, not a workflow-completion or lifetime guarantee.
struct PauseSnapshot {
    std::size_t revision; ///< State commit counter; wraparound is unsupported.
    bool root_created; ///< Whether the root was ever registered.
    GatePhase phase;
    std::optional<PauseEpoch> paused_epoch; ///< Present in Paused until that barrier ends.
    std::optional<PauseOrigin> origin; ///< Empty while the gate is Open.
    std::optional<std::size_t> step_round; ///< Initial SingleStep is 0; Continuous clears it.
    std::size_t waiting_branch_count; ///< Linked waiters; excludes completions already claimed.
    std::size_t live_branch_count; ///< Barrier participants, not live records or tasks.
};

/// Optional callbacks for entry into the final phase of a committed transaction.
/// Count, revision, or epoch updates within the same phase produce no callback.
/// Invoked outside the state lock; calls may overlap or arrive out of order.
/// Callbacks must be thread-safe and nonblocking; they must not synchronously reenter
/// Pause or destroy the source. The reported phase may no longer be current.
struct PausePhaseNotifications {
    std::move_only_function<void() noexcept> on_open_changed;
    std::move_only_function<void() noexcept> on_collecting_changed;
    std::move_only_function<void() noexcept> on_paused_changed;
};

namespace detail {

// Keep invariant enforcement active when assertions are disabled.
inline void require(bool condition) noexcept
{
    assert(condition);
    if (!condition) std::terminate();
}

inline std::lock_guard<std::mutex> lock_state(std::mutex& mutex) noexcept
{
    return std::lock_guard<std::mutex>{mutex};
}

template<class Node>
struct IntrusiveHook {
    Node* previous = nullptr;
    Node* next = nullptr;
};

// Non-owning. Protect shared lists with the state mutex; completion batches are private.
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
        // An O(1) hook check, not proof of membership: foreign linked nodes can also pass.
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
struct BranchRecord : IntrusiveHook<BranchRecord> {
    bool participates = false; // Counted in live_branch_count.
    bool arrival_pending = false; // Owes an arrival in this collecting round.
    PauseWaiter* waiter = nullptr; // Non-null only while linked in shared Waiting.
    std::size_t live_children = 0; // Direct child records not yet retired.
    BranchRecord* parent = nullptr; // Borrowed parent; must outlive this record.
};

enum class OperationPhase {
    Registering, CancelRequested, Waiting, CompletingValue, CompletingStopped
};
enum class Completion { None, InlineValue, ResumedValue, Stopped };

struct PauseWaiter : IntrusiveHook<PauseWaiter> {
    BranchRecord* branch_record;
    OperationPhase phase = OperationPhase::Registering;
    void (*reset_callback)(PauseWaiter*) noexcept;
    void (*deliver)(PauseWaiter*, Completion) noexcept;

    PauseWaiter(BranchRecord* branch, void (*reset)(PauseWaiter*) noexcept,
                void (*complete)(PauseWaiter*, Completion) noexcept) noexcept
        : branch_record(branch), reset_callback(reset), deliver(complete)
    {}
};

// Mutable shared state is protected by mutex.
// Prepare records, register/unregister callbacks, schedule, complete receivers, and invoke
// notifications outside the lock.
struct PauseStateData {
    mutable std::mutex mutex;
    PausePhaseNotifications* notifications;
    bool root_created = false;
    GatePhase phase = GatePhase::Open;
    std::size_t revision = 0;
    std::optional<PauseEpoch> paused_epoch;
    std::optional<PauseOrigin> origin;
    std::optional<std::size_t> step_round;
    std::size_t live_branch_count = 0;
    std::size_t pending_arrivals = 0;
    IntrusiveList<BranchRecord> branches;
    IntrusiveList<PauseWaiter> waiters;

    explicit PauseStateData(PausePhaseNotifications& phase_notifications) noexcept
        : notifications(&phase_notifications) {}

    // Dispatch the committed phase; do not reread a phase that another transaction may change.
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

// The caller must hold the corresponding state mutex for every *_locked helper.
inline void require_registered_locked(const PauseStateData& state,
                                      const BranchRecord& branch) noexcept
{
    require(state.branches.contains_link(branch));
}

// Debug validation holds the state lock and takes O(R^2 + W), for R records and W waiters.
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

inline void begin_collecting_locked(PauseStateData& state, PauseOrigin origin) noexcept
{
    require(state.phase == GatePhase::Open);
    state.phase = GatePhase::Collecting;
    state.origin = origin;
    state.pending_arrivals = state.live_branch_count;
    for (auto* b = state.branches.front(); b; b = b->next)
        b->arrival_pending = b->participates;
}

inline void satisfy_arrival_once_locked(PauseStateData& state, BranchRecord& branch) noexcept
{
    if (!branch.arrival_pending) return;
    require(state.phase == GatePhase::Collecting && state.pending_arrivals > 0);
    branch.arrival_pending = false;
    --state.pending_arrivals;
}

// Withdraw participation without retiring the record or changing its parent relationship.
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

// The last child retirement restores its parent. A Paused gate must collect again
// and invalidate the old epoch before the parent can arrive.
inline void return_parent_responsibility_locked(PauseStateData& state,
                                               BranchRecord& parent) noexcept
{
    require_registered_locked(state, parent);
    require(parent.live_children == 0 && !parent.participates);
    require(!parent.arrival_pending && parent.waiter == nullptr);
    if (state.phase == GatePhase::Paused) {
        state.phase = GatePhase::Collecting;
        state.paused_epoch.reset();
    }
    parent.participates = true;
    ++state.live_branch_count;
    if (state.phase == GatePhase::Collecting) {
        parent.arrival_pending = true;
        ++state.pending_arrivals;
    }
}

// Commit once per complete transaction, using the phase sampled before mutation.
// Report only a change to the final phase.
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

// A stopped reschedule must withdraw any next-round obligation before frame destruction.
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

// Claim completion, unlink the waiter, and clear branch.waiter under the same lock.
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
    // Unregister the entire batch before any receiver can reenter or destroy a completed node.
    for (auto* w = batch.front(); w; w = w->next) w->reset_callback(w);
    while (auto* w = batch.pop_front()) {
        w->deliver(w, Completion::ResumedValue); // w may now be destroyed.
    }
}

struct InlinePass {};
struct ResumedPass {};

// Type checks cannot verify the actual starting context or a type-erased scheduler backend.
template<class Env>
concept PauseEnvironment = requires(const Env& env) {
    { stdexec::get_start_scheduler(env) } -> stdexec::scheduler;
    requires std::same_as<std::remove_cvref_t<decltype(stdexec::get_stop_token(env))>,
                          stdexec::inplace_stop_token>;
};

template<class Receiver>
struct RawSuspendOperation : PauseWaiter {
    using operation_state_concept = stdexec::operation_state_tag;

    struct StopFn {
        RawSuspendOperation* operation;
        void operator()() noexcept { operation->on_stop(); }
    };

    PauseStateData* state;
    PausePointInfo info;
    Receiver receiver;
    stdexec::inplace_stop_token token;
    // Declared last so callback destruction precedes receiver destruction.
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
        // Unregister outside the state lock: a callback on another thread may be waiting for it.
        static_cast<RawSuspendOperation*>(waiter)->callback.reset();
    }
    static void complete(PauseWaiter* waiter, Completion result) noexcept
    {
        // A terminal completion may destroy self; do not access it afterward.
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
        auto* const s = state;
        auto* const b = branch_record;
        Completion own = Completion::None;
        bool changed = false;
        std::optional<GatePhase> entered_phase;

        // Registration may call back synchronously; start handles registration-time cancellation.
        callback.emplace(token, StopFn{this});
        {
            auto lock = lock_state(s->mutex);
            require_registered_locked(*s, *b);
            const auto previous_phase = s->phase;
            require(b->waiter == nullptr && b->live_children == 0);
            if (phase == OperationPhase::CancelRequested) {
                phase = OperationPhase::CompletingStopped;
                own = Completion::Stopped;
                changed = withdraw_locked(*s, *b);
            } else {
                require(phase == OperationPhase::Registering);
                const bool was_participating = b->participates;
                if (!was_participating) {
                    b->participates = true;
                    ++s->live_branch_count;
                    changed = true;
                }
                if (s->phase == GatePhase::Open) {
                    phase = OperationPhase::CompletingValue;
                    own = Completion::InlineValue;
                } else {
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
        // Publishing Waiting allows concurrent completion and destruction.
        // After unlocking, start must not access this unless it owns completion; notify via locals.
        if (changed) s->notify_phase_changed(entered_phase);
        if (own == Completion::None) return;
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
                // The callback may still be under construction. Record stop; start delivers
                // completion.
                phase = OperationPhase::CancelRequested;
                return;
            }
            if (!claim_waiter_locked(*s, *this, OperationPhase::CompletingStopped)) return;
            withdraw_locked(*s, *branch_record);
            entered_phase = commit_change_locked(*s, previous_phase);
        }
        s->notify_phase_changed(entered_phase);
        // Do not reset the active callback here; completion may destroy this operation.
        stdexec::set_stopped(std::move(receiver));
    }
};

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

// Account=true withdraws on stopped; false only wraps the affine adapter.
// Both roles terminate on internal error completions.
template<class Inner, bool Account>
struct CompletionPolicySender {
    using sender_concept = stdexec::sender_tag;
    PauseStateData* state;
    BranchRecord* branch;
    Inner inner;

    struct Attrs {
        stdexec::env_of_t<Inner> inner;
        // Only value promises affinity to the starting resource; stopped may run on a stop thread.
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
        // The value domain must match the starting scheduler, including native thread pools.
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
            return query(inner, args...);
        }
    };
    auto get_env() const noexcept { return Attrs{stdexec::get_env(inner)}; }

    template<class, class... Env>
    static consteval auto get_completion_signatures()
    {
        // Keep error signatures even though set_error terminates at run time.
        return stdexec::get_completion_signatures<Inner, Env...>();
    }

    template<class Receiver>
    struct PolicyReceiver {
        using receiver_concept = stdexec::receiver_tag;
        PauseStateData* state;
        BranchRecord* branch;
        Receiver receiver;
        decltype(auto) get_env() const noexcept { return stdexec::get_env(receiver); }
        void set_value() noexcept { stdexec::set_value(std::move(receiver)); }
        void set_stopped() noexcept
        {
            // Withdraw before forwarding completion; cleanup must not depend on frame destruction.
            if constexpr (Account) withdraw_and_notify(state, branch);
            stdexec::set_stopped(std::move(receiver));
        }
        template<class Error>
        [[noreturn]] void set_error(Error&&) noexcept { std::terminate(); }
    };

    template<class Receiver>
        requires PauseEnvironment<stdexec::env_of_t<Receiver>>
    auto connect(Receiver receiver) &&
    {
        return stdexec::connect(std::move(inner),
            PolicyReceiver<Receiver>{state, branch, std::move(receiver)});
    }

    // The default task customization adds a return hop for stopped, but none for value.
    auto affine() && requires Account
    {
        auto adapted = stdexec::let_stopped(std::move(*this), [] {
            return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
                [](auto scheduler) {
                    return stdexec::let_value(
                        // Shield only the hop delivering an already selected stopped result from
                        // cancellation.
                        stdexec::unstoppable(stdexec::schedule(scheduler)),
                        [] { return stdexec::just_stopped(); });
                });
        });
        return CompletionPolicySender<decltype(adapted), false>{nullptr, nullptr, std::move(adapted)};
    }
};

} // namespace detail

class PauseSource;

/// Owns the pause responsibility of one sequential path and borrows its PauseSource.
/// Serialize uses, moves, and destruction. Keep the branch and source valid throughout
/// public pause operations; a parent record must outlive every direct child record.
class PauseBranch {
public:
    PauseBranch(const PauseBranch&) = delete;
    PauseBranch& operator=(const PauseBranch&) = delete;
    PauseBranch(PauseBranch&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)), record_(std::move(other.record_))
    {}
    PauseBranch& operator=(PauseBranch&&) = delete;
    /// Retire the record without cancelling or joining work.
    /// @pre No active public pause operation borrows the record, and all direct children
    /// have retired.
    ~PauseBranch() noexcept
    {
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
                detail::require(parent->live_children > 0);
                --parent->live_children;
                b.parent = nullptr;
                if (parent->live_children == 0)
                    detail::return_parent_responsibility_locked(*s, *parent);
            }
            // Commit after the parent is restored; never publish a transient empty Paused state.
            entered_phase = detail::commit_change_locked(*s, previous_phase);
        }
        s->notify_phase_changed(entered_phase);
    }

    /// Create a lazy sender borrowing this branch and its source.
    /// Value completes inline while Open; a resumed value uses the starting scheduler.
    /// Accepted cancellation completes with stopped. Construction and public connect may throw;
    /// internal error completions after start terminate the process.
    [[nodiscard]] auto suspend_point(PausePointInfo info = {}) &
    {
        auto* s = state_;
        auto* b = record_.get();
        detail::require(s != nullptr && b != nullptr);
        auto routed = stdexec::let_value(detail::RawSuspendSender{s, b, info},
            [](auto route) {
                // The Open value path does not query a scheduler; only a resumed value is
                // scheduled.
                if constexpr (std::same_as<decltype(route), detail::InlinePass>) {
                    return stdexec::just();
                } else {
                    static_assert(std::same_as<decltype(route), detail::ResumedPass>);
                    return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
                        [](auto scheduler) {
                            return stdexec::schedule(scheduler);
                        });
                }
            });
        return detail::CompletionPolicySender<decltype(routed), true>{s, b, std::move(routed)};
    }
    auto suspend_point(PausePointInfo = {}) && = delete;

    /// Transfer responsibility to N child branches without starting or waiting for tasks.
    /// N == 0 returns InvalidForkCount; catchable allocation failures return ResourceExhausted.
    /// Successful transfer requires participation, no waiter or live children, and a gate
    /// that is not Paused; otherwise state validation returns InvalidBranchState.
    /// The last child retirement restores this branch; checkpoint before more business work.
    template<std::size_t N>
    [[nodiscard]] std::expected<std::array<PauseBranch, N>, PauseError>
    fork_and_block_self() &
    {
        if constexpr (N == 0) {
            return std::unexpected(PauseError::InvalidForkCount);
        } else {
            try {
                auto children = []<std::size_t... Index>(std::index_sequence<Index...>) {
                    return std::array<PauseBranch, N>{((void)Index, prepare())...};
                }(std::make_index_sequence<N>{});
                return commit_prepared_fork(std::move(children));
            } catch (const std::bad_alloc&) {
                return std::unexpected(PauseError::ResourceExhausted);
            }
        }
    }

    /// Runtime-width fork with the same transfer rules as the fixed-width overload.
    /// Zero count returns InvalidForkCount; catchable allocation or length failures return
    /// ResourceExhausted. MSVC debug iterator-proxy allocation inside noexcept may terminate;
    /// see the exception-safety contract in docs/design.md.
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
    detail::PauseStateData* state_;
    std::unique_ptr<detail::BranchRecord> record_;

    PauseBranch(detail::PauseStateData* state,
                std::unique_ptr<detail::BranchRecord> record) noexcept
        : state_(state), record_(std::move(record))
    {}
    static PauseBranch prepare()
    {
        return {nullptr, std::make_unique<detail::BranchRecord>()};
    }
    // Records are prepared outside the lock. Transfer and commit as one transaction,
    // without publishing an intermediate empty barrier.
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
                b.arrival_pending = s->phase == GatePhase::Collecting;
                child.state_ = s;
                s->branches.push_back(b);
                ++s->live_branch_count;
                if (b.arrival_pending) ++s->pending_arrivals;
            }
            entered_phase = detail::commit_change_locked(*s, previous_phase);
        }
        s->notify_phase_changed(entered_phase);
        return std::expected<Container, PauseError>{std::in_place, std::move(children)};
    }
};

/// A shared pause gate with a stable address; owns neither tasks nor threads.
/// Before destruction, release all branches and operations and finish all control,
/// observation, callback, start, and notification calls. Control and snapshot calls
/// may run concurrently while the source remains alive.
class PauseSource {
public:
    /// Start Open. Initial SingleStep arms round 0 when the root is registered.
    /// All notification callbacks may be empty.
    PauseSource(AdvanceMode initial_mode, PausePhaseNotifications notifications = {})
        : initial_mode_(initial_mode), notifications_(std::move(notifications)),
          state_(notifications_)
    {}
    /// Does not cancel, complete waiters, or join callers; the host must ensure quiescence.
    ~PauseSource() noexcept
    {
        assert(state_.branches.empty() && state_.waiters.empty());
        assert(state_.live_branch_count == 0 && state_.pending_arrivals == 0);
    }
    PauseSource(const PauseSource&) = delete;
    PauseSource& operator=(const PauseSource&) = delete;
    PauseSource(PauseSource&&) = delete;
    PauseSource& operator=(PauseSource&&) = delete;

    /// Register the root without starting work. At most one registration succeeds per source.
    [[nodiscard]] std::expected<PauseBranch, PauseError> make_root_branch() &
    {
        return make_branch_impl(true);
    }
    std::expected<PauseBranch, PauseError> make_root_branch() && = delete;

#if PAUSE_ENABLE_INDEPENDENT_BRANCH
    /// Register an independent path after root registration, while the gate is Open.
    /// Available only when PAUSE_ENABLE_INDEPENDENT_BRANCH is enabled.
    [[nodiscard]] std::expected<PauseBranch, PauseError> make_branch() &
    {
        return make_branch_impl(false);
    }
    std::expected<PauseBranch, PauseError> make_branch() && = delete;
#endif

    /// Close the gate and collect arrivals. Success does not imply the gate is already Paused.
    /// With no participants, the same transaction enters an empty Paused state.
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

    /// Resume only the current paused epoch from this source.
    /// Continuous opens the gate; SingleStep arms the next round before releasing waiters.
    /// Success does not imply public operations or business work have completed.
    /// May block while unregistering a callback executing on another thread.
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
                s->step_round = s->step_round.value_or(0) + 1;
                detail::begin_collecting_locked(*s, PauseOrigin::SingleStep);
            }
            entered_phase = detail::commit_change_locked(*s, previous_phase);
        }
        s->notify_phase_changed(entered_phase);
        detail::complete_value_batch(batch);
        return {};
    }
    std::expected<void, PauseError> resume(PauseEpoch, AdvanceMode) && = delete;

    /// Resume the epoch observed during this call; concurrent changes are not retried.
    /// Only the epoch lookup is O(1). Use the explicit overload to bind an earlier pause.
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

    /// Take a consistent snapshot under the lock in O(W), where W is the waiter count.
    /// The state may change immediately after the lock is released.
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
    // Keep notifications_ alive throughout state_ construction and destruction.
    const AdvanceMode initial_mode_;
    PausePhaseNotifications notifications_;
    detail::PauseStateData state_;

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
            return std::expected<PauseBranch, PauseError>{std::in_place, std::move(branch)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PauseError::ResourceExhausted);
        }
    }
};

} // namespace coop_pause

#endif // PAUSE_HPP_
