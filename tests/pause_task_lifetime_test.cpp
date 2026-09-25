/* 测试项目：pause_task_lifetime_test
 * 性质: 真实 Pause 记录、task 帧与 when_all/optional 宿主取消的生命周期测试.
 * 测试项目:
 * - Pause stopped 转 optional 后保留子帧: callback 先撤销参与责任, 但记录/帧暂未析构;
 *   sibling 可继续暂停/恢复, 被保留记录最终退休时只提交一次 revision.
 * - 验证子记录按本用例的 B、A 次序退休, 均早于父重入和父记录退休;
 *   父重入重新承担一次责任, 仅未取消的子分支及父分支继续业务, 最终 live 归零.
 * - optional 宿主 starts_on 预取消: 包装 task 和其子 task 均未进入,
 *   stopped 传到 when_all 并取消正在 Pause 等待的兄弟, 所有记录最终退休.
 * - optional 已将前一 stopped 转为空值后发生新的可取消等待: 后续 scheduler 的 stopped
 *   仍传到 when_all 并取消兄弟, 不被先前 optional 转换吞掉, 业务不再继续.
 * 运行: ctest --test-dir <build> -R '^pause_task_lifetime_test$' --output-on-failure.
 * CLI: --iterations N (默认 100, 范围 1..1000000), 每轮执行 3 个生命周期场景.
 * 边界: 通过公开 snapshot、帧析构见证和独立协调线程观察时序; 不访问 Pause 私有记录,
 *   不使用 sleep 建立顺序, 不让完成线程等待自身 callback 析构.
 */
#include "pause_sender_test_support.hpp"

#include <array>
#include <optional>

// Real Pause records, task frames, and when_all cancellation. The separate
// scheduling loops are started by the coordinator, so no sleep establishes an
// ordering and no completion thread waits for its own callback destruction.
namespace {
using namespace pause_sender_test;

struct Signal {
    std::atomic<std::size_t> sequence{0};
    void notify() noexcept
    {
        sequence.fetch_add(1, std::memory_order_release);
        sequence.notify_all();
    }
};

template<class Predicate>
coop_pause::PauseSnapshot await_snapshot(Signal& changed, const coop_pause::PauseSource& source,
                                    Predicate predicate)
{
    for (;;) {
        const auto observed = changed.sequence.load(std::memory_order_acquire);
        auto snapshot = source.snapshot();
        if (predicate(snapshot)) return snapshot;
        changed.sequence.wait(observed, std::memory_order_acquire);
    }
}

struct Retirement {
    std::atomic<bool> retired{false};
    coop_pause::PauseSnapshot snapshot{};
    int order = 0;
};

struct Lifetime {
    coop_pause::PauseSource* source;
    std::array<Retirement, 3> records; // A, B, parent.
    std::atomic<int> order{0};
    std::array<std::atomic<bool>, 2> entered{};
    std::array<std::atomic<int>, 2> business{};
    std::atomic<bool> wrapper_entered{false};
    std::atomic<int> parent_business{0};
    std::size_t before_reentry = 0;
    std::size_t after_reentry = 0;
};

struct Witness {
    Lifetime* lifetime;
    int index;
    Witness(Lifetime& state, int id) noexcept : lifetime(&state), index(id) {}
    Witness(const Witness&) = delete;
    Witness(Witness&& other) noexcept
        : lifetime(std::exchange(other.lifetime, nullptr)), index(other.index)
    {}
    ~Witness()
    {
        if (!lifetime) return;
        auto& event = lifetime->records[index];
        event.snapshot = lifetime->source->snapshot();
        event.order = lifetime->order.fetch_add(1) + 1;
        CHECK(!event.retired.exchange(true, std::memory_order_release));
    }
};

struct OwnedBranch {
    // Reverse member destruction retires the actual Pause record first, then
    // publishes the owning frame's witness. No private Pause state is queried.
    Witness witness;
    coop_pause::PauseBranch branch;
    OwnedBranch(Lifetime& state, int index, coop_pause::PauseBranch value) noexcept
        : witness(state, index), branch(std::move(value))
    {}
    OwnedBranch(const OwnedBranch&) = delete;
    OwnedBranch(OwnedBranch&&) noexcept = default;
};

struct Host {
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    ReceiverState receiver{&scheduler, stop.get_token()};

    template<class Sender>
    void run(Sender sender)
    {
        auto operation = stdexec::connect(stdexec::starts_on(
            CountingScheduler{&scheduler}, std::move(sender)), Receiver{&receiver});
        stdexec::start(operation);
        scheduler.thread = std::this_thread::get_id();
        scheduler.loop.run();
    }
};

struct ManualEvent {
    std::latch published{1};
    void* operation = nullptr;
    void (*complete)(void*) noexcept = nullptr;
    void finish() noexcept
    {
        published.wait();
        complete(operation);
    }
};

struct EventSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;
    ManualEvent* event;
    template<class Receiver>
    struct Operation {
        ManualEvent* event;
        Receiver receiver;
        void start() & noexcept
        {
            auto* local = event;
            local->operation = this;
            local->complete = [](void* pointer) noexcept {
                auto* self = static_cast<Operation*>(pointer);
                stdexec::set_value(std::move(self->receiver));
            };
            local->published.count_down();
        }
    };
    template<class Receiver>
    auto connect(Receiver receiver) const
    {
        return Operation<Receiver>{event, std::move(receiver)};
    }
};

stdexec::task<int> gated_child(OwnedBranch owned, Lifetime& lifetime,
                              int index, ManualEvent& event)
{
    lifetime.entered[index].store(true, std::memory_order_release);
    co_await owned.branch.suspend_point();
    co_await EventSender{&event};
    co_await owned.branch.suspend_point();
    ++lifetime.business[index];
    co_return 42;
}

stdexec::task<int> entry_child(OwnedBranch owned, Lifetime& lifetime, int index,
                              bool local_stopped)
{
    lifetime.entered[index].store(true, std::memory_order_release);
    co_await owned.branch.suspend_point();
    if (local_stopped) co_await stdexec::just_stopped();
    ++lifetime.business[index];
    co_return 42;
}

stdexec::task<std::optional<int>> optional_task(stdexec::task<int> inner, Lifetime& lifetime)
{
    lifetime.wrapper_entered.store(true, std::memory_order_release);
    auto value = co_await stdexec::stopped_as_optional(std::move(inner));
    co_return value;
}

stdexec::task<void> retained_parent(OwnedBranch parent, Lifetime& lifetime,
                                   ManualEvent& a, ManualEvent& b,
                                   stdexec::inplace_stop_token independent_a,
                                   std::latch& converted)
{
    co_await parent.branch.suspend_point();
    auto children = parent.branch.fork_and_block_self<2>();
    CHECK(children);
    auto first = stdexec::write_env(
        stdexec::stopped_as_optional(gated_child(
            OwnedBranch{lifetime, 0, std::move((*children)[0])}, lifetime, 0, a))
            | stdexec::then([&](std::optional<int> result) {
                  CHECK(!result);
                  converted.count_down();
              }),
        stdexec::prop{stdexec::get_stop_token, independent_a});
    co_await stdexec::when_all(std::move(first), gated_child(
        OwnedBranch{lifetime, 1, std::move((*children)[1])}, lifetime, 1, b));
    CHECK(lifetime.records[0].retired.load(std::memory_order_acquire));
    CHECK(lifetime.records[1].retired.load(std::memory_order_acquire));
    CHECK(lifetime.source->snapshot().live_branch_count == 0);
    lifetime.before_reentry = lifetime.source->snapshot().revision;
    co_await parent.branch.suspend_point();
    lifetime.after_reentry = lifetime.source->snapshot().revision;
    CHECK(lifetime.source->snapshot().live_branch_count == 1);
    ++lifetime.parent_business;
}

void retained_pause_stopped_frame()
{
    Signal changed;
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous, [&]() noexcept { changed.notify(); });
    Lifetime lifetime{&source};
    auto root = source.make_root_branch();
    CHECK(root);
    Host host;
    stdexec::inplace_stop_source independent_a;
    ManualEvent a, b;
    std::latch converted{1};
    std::jthread runner([&] {
        host.run(retained_parent(OwnedBranch{lifetime, 2, std::move(*root)}, lifetime,
                                 a, b, independent_a.get_token(), converted));
    });
    a.published.wait();
    b.published.wait();
    CHECK(source.request_pause());
    a.finish();
    b.finish();
    const auto paused = await_snapshot(changed, source, [](const auto& state) {
        return state.phase == coop_pause::GatePhase::Paused;
    });
    CHECK(paused.live_branch_count == 2 && paused.waiting_branch_count == 2);
    CHECK(!independent_a.request_stop());
    converted.wait();
    const auto withdrawn = source.snapshot();
    CHECK(withdrawn.phase == coop_pause::GatePhase::Paused);
    CHECK(withdrawn.live_branch_count == 1 && withdrawn.waiting_branch_count == 1);
    CHECK(withdrawn.paused_epoch == paused.paused_epoch);
    CHECK(withdrawn.revision == paused.revision + 1);
    // A's callback already withdrew responsibility, while its owned branch
    // record and frame remain inside the stopped-as-value when_all operation.
    CHECK(!lifetime.records[0].retired.load(std::memory_order_acquire));
    CHECK(!lifetime.records[1].retired.load(std::memory_order_acquire));
    CHECK(!lifetime.records[2].retired.load(std::memory_order_acquire));
    CHECK(lifetime.parent_business == 0);
    CHECK(source.resume(*withdrawn.paused_epoch, coop_pause::AdvanceMode::Continuous));
    runner.join();
    CHECK(host.receiver.result == Completion::Value);
    const auto& retired_a = lifetime.records[0];
    const auto& retired_b = lifetime.records[1];
    const auto& retired_parent = lifetime.records[2];
    CHECK(retired_a.retired && retired_b.retired && retired_parent.retired);
    CHECK(retired_b.order < retired_a.order && retired_a.order < retired_parent.order);
    CHECK(retired_b.snapshot.live_branch_count == 0);
    CHECK(retired_a.snapshot.live_branch_count == 0);
    // Resume, B record retirement, then retained A record retirement each
    // commit exactly once; withdrawing A earlier did not retire its record.
    CHECK(retired_b.snapshot.revision == withdrawn.revision + 2);
    CHECK(retired_a.snapshot.revision == retired_b.snapshot.revision + 1);
    CHECK(lifetime.before_reentry == retired_a.snapshot.revision);
    CHECK(lifetime.after_reentry == lifetime.before_reentry + 1);
    CHECK(retired_parent.snapshot.revision == lifetime.after_reentry + 1);
    CHECK(lifetime.business[0] == 0 && lifetime.business[1] == 1);
    CHECK(lifetime.parent_business == 1 && source.snapshot().live_branch_count == 0);
}

template<class First, class Second>
stdexec::task<void> join_without_business(First first, Second second,
                                        std::atomic<bool>& continued)
{
    co_await stdexec::when_all(std::move(first), std::move(second));
    continued.store(true, std::memory_order_release);
}

void optional_host_precancelled()
{
    Signal changed;
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous, [&]() noexcept { changed.notify(); });
    Lifetime lifetime{&source};
    auto root = source.make_root_branch();
    CHECK(root);
    auto children = root->fork_and_block_self<2>();
    CHECK(children && source.request_pause());
    Host host;
    stdexec::run_loop first_loop;
    stdexec::inplace_stop_source independent_a;
    CHECK(!independent_a.request_stop());
    std::atomic<bool> continued{false};
    auto first = stdexec::write_env(stdexec::starts_on(first_loop.get_scheduler(), optional_task(
        entry_child(OwnedBranch{lifetime, 0, std::move((*children)[0])}, lifetime, 0, false), lifetime)),
        stdexec::prop{stdexec::get_stop_token, independent_a.get_token()});
    auto second = entry_child(OwnedBranch{lifetime, 1, std::move((*children)[1])}, lifetime, 1, false);
    std::jthread runner([&] {
        host.run(join_without_business(std::move(first), std::move(second), continued));
    });
    const auto waiting = await_snapshot(changed, source, [](const auto& state) {
        return state.waiting_branch_count == 1;
    });
    CHECK(waiting.phase == coop_pause::GatePhase::Collecting && waiting.live_branch_count == 2);
    CHECK(lifetime.entered[1] && !lifetime.entered[0] && !lifetime.wrapper_entered);
    std::jthread first_runner([&] { first_loop.run(); });
    // The pre-cancelled starts_on stops before optional_task enters. That
    // stopped remains visible to when_all, which cancels B's live Pause waiter.
    runner.join();
    first_loop.finish();
    first_runner.join();
    CHECK(host.receiver.result == Completion::Stopped && !continued);
    CHECK(!lifetime.entered[0] && !lifetime.wrapper_entered);
    CHECK(lifetime.business[0] == 0 && lifetime.business[1] == 0);
    CHECK(lifetime.records[0].retired && lifetime.records[1].retired);
    CHECK(source.snapshot().phase == coop_pause::GatePhase::Paused);
    CHECK(source.snapshot().live_branch_count == 0 && source.snapshot().waiting_branch_count == 0);
}

void optional_host_later_cancellable_wait()
{
    Signal changed;
    coop_pause::PauseSource source(coop_pause::AdvanceMode::Continuous, [&]() noexcept { changed.notify(); });
    Lifetime lifetime{&source};
    auto root = source.make_root_branch();
    CHECK(root);
    auto children = root->fork_and_block_self<2>();
    CHECK(children);
    Host host;
    stdexec::run_loop subsequent_loop;
    stdexec::inplace_stop_source independent_a;
    ManualEvent b;
    std::latch optional_completed{1};
    std::atomic<bool> continued{false};
    auto first = stdexec::write_env(stdexec::let_value(optional_task(
        entry_child(OwnedBranch{lifetime, 0, std::move((*children)[0])}, lifetime, 0, true), lifetime),
        [&](std::optional<int>& value) {
            CHECK(!value);
            CHECK(lifetime.records[0].retired.load(std::memory_order_acquire));
            optional_completed.count_down();
            return stdexec::schedule(subsequent_loop.get_scheduler());
        }), stdexec::prop{stdexec::get_stop_token, independent_a.get_token()});
    auto second = gated_child(OwnedBranch{lifetime, 1, std::move((*children)[1])}, lifetime, 1, b);
    std::jthread runner([&] {
        host.run(join_without_business(std::move(first), std::move(second), continued));
    });
    b.published.wait();
    optional_completed.wait();
    CHECK(lifetime.wrapper_entered && lifetime.entered[0]);
    CHECK(source.snapshot().live_branch_count == 1);
    CHECK(source.request_pause());
    b.finish();
    const auto paused = await_snapshot(changed, source, [](const auto& state) {
        return state.phase == coop_pause::GatePhase::Paused;
    });
    CHECK(paused.live_branch_count == 1 && paused.waiting_branch_count == 1);
    CHECK(!independent_a.request_stop());
    // A's new scheduler operation has not executed: the optional conversion
    // does not swallow this later stopped, and it is not a host affine hop.
    CHECK(source.snapshot().waiting_branch_count == 1);
    CHECK(host.receiver.result == Completion::None);
    std::jthread subsequent_runner([&] { subsequent_loop.run(); });
    runner.join();
    subsequent_loop.finish();
    subsequent_runner.join();
    CHECK(host.receiver.result == Completion::Stopped && !continued);
    CHECK(lifetime.business[0] == 0 && lifetime.business[1] == 0);
    CHECK(lifetime.records[0].retired && lifetime.records[1].retired);
    CHECK(source.snapshot().phase == coop_pause::GatePhase::Paused);
    CHECK(source.snapshot().live_branch_count == 0 && source.snapshot().waiting_branch_count == 0);
}
} // namespace

int main(int argc, char** argv)
{
    const int count = iterations(argc, argv);
    for (int iteration = 0; iteration != count; ++iteration) {
        retained_pause_stopped_frame();
        optional_host_precancelled();
        optional_host_later_cancellable_wait();
    }
    std::printf("PASS: 3 real Pause task lifetime/optional-host cancellation cases x %d iterations\n", count);
    std::puts("callback withdrawal precedes retained record retirement; children retire before parent reentry;");
    std::puts("pre-cancelled starts_on and later cancellable wait propagate stopped to a waiting sibling.");
}
