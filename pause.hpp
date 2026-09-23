#ifndef PAUSE_HPP_
#define PAUSE_HPP_

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

namespace pause {

enum class AdvanceMode { Continuous, SingleStep };
enum class PauseOrigin { ControlRequest, SingleStep };
enum class GatePhase { Open, Collecting, Paused };

struct PausePointInfo {
    std::size_t tag = 0;
};

struct PauseEpoch {
    std::size_t value;
    friend bool operator==(PauseEpoch, PauseEpoch) = default;
};

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

struct PauseSnapshot {
    std::size_t revision;
    bool root_created;
    GatePhase phase;
    std::optional<PauseEpoch> paused_epoch;
    std::optional<PauseOrigin> origin;
    std::optional<std::size_t> step_round;
    std::size_t waiting_branch_count;
    std::size_t live_branch_count;
};

using StateChangedNotifier = std::move_only_function<void() noexcept>;

namespace detail {

inline void require(bool condition) noexcept
{
    assert(condition);
    if (!condition) std::terminate();
}

inline std::lock_guard<std::mutex> lock_state(std::mutex& mutex) noexcept
{
    // A broken system synchronization primitive is not a recoverable API error.
    return std::lock_guard<std::mutex>{mutex};
}

template<class Node>
struct IntrusiveHook {
    Node* previous = nullptr;
    Node* next = nullptr;
};

// Non-owning. A removed node can immediately use the same hook in a private
// completion batch. No allocation or user code runs while manipulating links.
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
    bool participates = false;
    bool arrival_pending = false;
    PauseWaiter* waiter = nullptr;
    std::size_t live_children = 0;
    BranchRecord* parent = nullptr;
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

struct PauseStateData {
    mutable std::mutex mutex;
    StateChangedNotifier* notifier;
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

    explicit PauseStateData(StateChangedNotifier& notify) noexcept : notifier(&notify) {}
    void notify_changed() noexcept { (*notifier)(); }
};

inline void require_registered_locked(const PauseStateData& state,
                                      const BranchRecord& branch) noexcept
{
    require(state.branches.contains_link(branch));
}

// Reconstruct the invariants after each committed transaction in debug builds.
// This deliberately allocates nothing and does not call external code.
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

inline void commit_change_locked(PauseStateData& state) noexcept
{
    ++state.revision;
    if (state.phase == GatePhase::Collecting && state.pending_arrivals == 0) {
        state.phase = GatePhase::Paused;
        state.paused_epoch = PauseEpoch{state.revision};
    }
    assert_invariants_locked(state);
}

inline void withdraw_and_notify(PauseStateData* state, BranchRecord* branch) noexcept
{
    bool changed;
    {
        auto lock = lock_state(state->mutex);
        require_registered_locked(*state, *branch);
        changed = withdraw_locked(*state, *branch);
        if (changed) commit_change_locked(*state);
    }
    if (changed) state->notify_changed();
}

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
    // Quiesce the entire batch before any receiver can reenter or cancel.
    for (auto* w = batch.front(); w; w = w->next) w->reset_callback(w);
    while (auto* w = batch.pop_front()) {
        w->deliver(w, Completion::ResumedValue);
        // The terminal CPO may have destroyed w. Never read it again.
    }
}

struct InlinePass {};
struct ResumedPass {};

template<class Env>
concept PauseEnvironment = requires(const Env& env) {
    { stdexec::get_start_scheduler(env) } -> stdexec::scheduler;
    requires std::same_as<std::remove_cvref_t<decltype(stdexec::get_stop_token(env))>,
                          stdexec::inplace_stop_token>;
};

template<class Receiver>
struct RawSuspendOperation : PauseWaiter {
    struct StopFn {
        RawSuspendOperation* operation;
        void operator()() noexcept { operation->on_stop(); }
    };

    PauseStateData* state;
    PausePointInfo info;
    Receiver receiver;
    stdexec::inplace_stop_token token;
    // Last: unregister before any callback-accessed member is destroyed.
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
        static_cast<RawSuspendOperation*>(waiter)->callback.reset();
    }
    static void complete(PauseWaiter* waiter, Completion result) noexcept
    {
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

        // May call on_stop before emplace returns. Registering only records it.
        callback.emplace(token, StopFn{this});
        {
            auto lock = lock_state(s->mutex);
            require_registered_locked(*s, *b);
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
            if (changed) commit_change_locked(*s);
        }
        // Once Waiting is published, this operation may already be destroyed.
        // Only saved locals are usable unless start owns the completion.
        if (changed) s->notify_changed();
        if (own == Completion::None) return;
        callback.reset();
        complete(this, own);
    }
    void on_stop() noexcept
    {
        auto* const s = state;
        {
            auto lock = lock_state(s->mutex);
            if (phase == OperationPhase::Registering) {
                phase = OperationPhase::CancelRequested;
                return;
            }
            if (!claim_waiter_locked(*s, *this, OperationPhase::CompletingStopped)) return;
            withdraw_locked(*s, *branch_record);
            commit_change_locked(*s);
        }
        s->notify_changed();
        stdexec::set_stopped(std::move(receiver));
        // Includes the self-unregistration case: no access after completion.
    }
};

struct RawSuspendSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<
        stdexec::set_value_t(InlinePass), stdexec::set_value_t(ResumedPass),
        stdexec::set_stopped_t()>;
    PauseStateData* state;
    BranchRecord* branch;
    PausePointInfo info;

    template<class Receiver>
        requires PauseEnvironment<stdexec::env_of_t<Receiver>>
    auto connect(Receiver receiver) const
    {
        return RawSuspendOperation<Receiver>{state, branch, info, std::move(receiver)};
    }
};

// The same policy implementation serves the body and its outer host adapter.
// Only the body accounts stopped; both terminate on infrastructure errors.
template<class Inner, bool Account>
struct CompletionPolicySender {
    using sender_concept = stdexec::sender_tag;
    PauseStateData* state;
    BranchRecord* branch;
    Inner inner;

    struct Attrs {
        stdexec::env_of_t<Inner> inner;
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
        template<class Query, class... Args>
            requires (stdexec::forwarding_query(Query{})
                      && !(Account && std::same_as<Query,
                          exec::get_completion_behavior_t<stdexec::set_value_t>>)
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

    auto affine() && requires Account
    {
        auto adapted = stdexec::let_stopped(std::move(*this), [] {
            return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
                [](auto scheduler) {
                    return stdexec::let_value(
                        stdexec::unstoppable(stdexec::schedule(scheduler)),
                        [] { return stdexec::just_stopped(); });
                });
        });
        return CompletionPolicySender<decltype(adapted), false>{nullptr, nullptr, std::move(adapted)};
    }
};

} // namespace detail

class PauseSource;

class PauseBranch {
public:
    PauseBranch(const PauseBranch&) = delete;
    PauseBranch& operator=(const PauseBranch&) = delete;
    PauseBranch(PauseBranch&& other) noexcept
        : state_(std::exchange(other.state_, nullptr)), record_(std::move(other.record_))
    {}
    PauseBranch& operator=(PauseBranch&&) = delete;
    ~PauseBranch() noexcept
    {
        if (!state_ || !record_) return;
        auto* const s = state_;
        {
            auto lock = detail::lock_state(s->mutex);
            auto& b = *record_;
            detail::require_registered_locked(*s, b);
            detail::require(b.waiter == nullptr && b.live_children == 0);
            detail::withdraw_locked(*s, b);
            s->branches.erase(b);
            if (b.parent) {
                detail::require(b.parent->live_children > 0);
                --b.parent->live_children;
                b.parent = nullptr;
            }
            detail::commit_change_locked(*s);
        }
        s->notify_changed();
        // record_ releases storage outside the lock.
    }

    [[nodiscard]] auto suspend_point(PausePointInfo info = {}) &
    {
        auto* s = state_;
        auto* b = record_.get();
        detail::require(s != nullptr && b != nullptr);
        auto routed = stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
            [s, b, info](auto scheduler) {
                return stdexec::let_value(detail::RawSuspendSender{s, b, info},
                    [scheduler = std::move(scheduler)](auto route) {
                        if constexpr (std::same_as<decltype(route), detail::InlinePass>) {
                            return stdexec::just();
                        } else {
                            static_assert(std::same_as<decltype(route), detail::ResumedPass>);
                            return stdexec::schedule(scheduler);
                        }
                    });
            });
        return detail::CompletionPolicySender<decltype(routed), true>{s, b, std::move(routed)};
    }
    auto suspend_point(PausePointInfo = {}) && = delete;

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
    template<class Container>
    std::expected<Container, PauseError> commit_prepared_fork(Container children) & noexcept
    {
        static_assert(std::is_nothrow_move_constructible_v<Container>);
        auto* const s = state_;
        if (!s || !record_) return std::unexpected(PauseError::InvalidBranchState);
        {
            auto lock = detail::lock_state(s->mutex);
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
            detail::commit_change_locked(*s);
        }
        s->notify_changed();
        return std::expected<Container, PauseError>{std::in_place, std::move(children)};
    }
};

class PauseSource {
public:
    PauseSource(AdvanceMode initial_mode, StateChangedNotifier notify_changed)
        : initial_mode_(initial_mode), notify_changed_(std::move(notify_changed)),
          state_(notify_changed_)
    {
        detail::require(static_cast<bool>(notify_changed_));
    }
    ~PauseSource() noexcept
    {
        // The owner must also join callers and notification tails; these
        // assertions cannot prove that external lifetime precondition.
        assert(state_.branches.empty() && state_.waiters.empty());
        assert(state_.live_branch_count == 0 && state_.pending_arrivals == 0);
    }
    PauseSource(const PauseSource&) = delete;
    PauseSource& operator=(const PauseSource&) = delete;
    PauseSource(PauseSource&&) = delete;
    PauseSource& operator=(PauseSource&&) = delete;

    [[nodiscard]] std::expected<PauseBranch, PauseError> make_root_branch() &
    {
        return make_branch_impl(true);
    }
    std::expected<PauseBranch, PauseError> make_root_branch() && = delete;
    [[nodiscard]] std::expected<PauseBranch, PauseError> make_branch() &
    {
        return make_branch_impl(false);
    }
    std::expected<PauseBranch, PauseError> make_branch() && = delete;

    [[nodiscard]] std::expected<void, PauseError> request_pause() &
    {
        auto* const s = &state_;
        {
            auto lock = detail::lock_state(s->mutex);
            if (!s->root_created) return std::unexpected(PauseError::RootNotCreated);
            if (s->phase == GatePhase::Collecting)
                return std::unexpected(PauseError::PauseAlreadyCollecting);
            if (s->phase == GatePhase::Paused) return std::unexpected(PauseError::AlreadyPaused);
            detail::begin_collecting_locked(*s, PauseOrigin::ControlRequest);
            detail::commit_change_locked(*s);
        }
        s->notify_changed();
        return {};
    }
    std::expected<void, PauseError> request_pause() && = delete;

    [[nodiscard]] std::expected<void, PauseError> resume(PauseEpoch epoch, AdvanceMode mode) &
    {
        auto* const s = &state_;
        detail::CompletionList batch;
        {
            auto lock = detail::lock_state(s->mutex);
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
            detail::commit_change_locked(*s);
        }
        s->notify_changed();
        detail::complete_value_batch(batch);
        return {};
    }
    std::expected<void, PauseError> resume(PauseEpoch, AdvanceMode) && = delete;

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
    const AdvanceMode initial_mode_;
    StateChangedNotifier notify_changed_;
    detail::PauseStateData state_;

    std::expected<PauseBranch, PauseError> make_branch_impl(bool root)
    {
        try {
            auto branch = PauseBranch::prepare();
            auto* const s = &state_;
            {
                auto lock = detail::lock_state(s->mutex);
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
                detail::commit_change_locked(*s);
            }
            s->notify_changed();
            return std::expected<PauseBranch, PauseError>{std::in_place, std::move(branch)};
        } catch (const std::bad_alloc&) {
            return std::unexpected(PauseError::ResourceExhausted);
        }
    }
};

} // namespace pause

#endif  // PAUSE_HPP_