/* 测试项目：pause_notification_matrix_test
 * 目的: 验证真实 Pause 通知与宿主观察者按 dirty/sequence 读取最新快照的协议.
 * 测试项:
 * - NOTIFY-01: 多次通知合并后两个观察者各读一次最新快照.
 * - NOTIFY-02: 宿主乱序/重复投递及额外唤醒不改变 Pause 状态.
 * - NOTIFY-03/04: 消费 dirty 后, snapshot 前/后发生新通知, dirty 必须保留.
 * - NOTIFY-05: 4 个调用者并发创建/退役独立分支, 两个观察者读取一致状态.
 * - NOTIFY-06: 不产生 Pause 状态变化也能可靠唤醒退出, 等待观察者调用尾部并 join.
 * 关键预期: 每项核对精确通知/读取次数, 快照各字段和 dirty 消费结果;
 * 每个观察者处理器串行, 两个观察者可并发读; 资源保留到真实线程退出.
 * 入口: PAUSE_BUILD_TESTS=ON 时注册同名 CTest.
 *   ctest --test-dir <build> -C <config> -R "^pause_notification_matrix_test$" --output-on-failure
 *   pause_notification_matrix_test --iterations 3
 * 参数: iterations 为 1..1000000, 默认每个稳定场景 ID 执行 3 次.
 * 边界: notifier 始终非阻塞且不重入 Pause, 所有测试闸门位于宿主投递侧;
 * receipt 只是宿主标记, 不能当作 epoch 授权恢复. 并发 notifier 重叠数仅报告,
 * 不假定线程 barrier 能保证短通知函数在每次运行中实际重叠.
 */
#include "pause.hpp"

#include <array>
#include <atomic>
#include <barrier>
#include <charconv>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <optional>
#include <string_view>
#include <thread>

namespace {
using coop_pause::AdvanceMode;
using coop_pause::GatePhase;
using coop_pause::PauseBranch;
using coop_pause::PauseSnapshot;
using coop_pause::PauseSource;

const char* current_case = "arguments";
int current_iteration = 0;
[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "%s iteration=%d line=%d CHECK failed: %s\n",
                 current_case, current_iteration, line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

PauseBranch take(std::expected<PauseBranch, coop_pause::PauseError> result)
{
    CHECK(result);
    return std::move(*result);
}

void equal_snapshot(const PauseSnapshot& actual, const PauseSnapshot& expected)
{
    CHECK(actual.revision == expected.revision);
    CHECK(actual.root_created == expected.root_created);
    CHECK(actual.phase == expected.phase);
    CHECK(actual.paused_epoch == expected.paused_epoch);
    CHECK(actual.origin == expected.origin);
    CHECK(actual.step_round == expected.step_round);
    CHECK(actual.waiting_branch_count == expected.waiting_branch_count);
    CHECK(actual.live_branch_count == expected.live_branch_count);
}

struct Mailbox {
    std::atomic<bool> dirty{false};
    std::atomic<std::uint64_t> sequence{0};
    std::atomic<std::uint64_t> receipt{0}; // Host delivery tag, never a Pause epoch.
    void signal() noexcept
    {
        dirty.store(true, std::memory_order_release);
        sequence.fetch_add(1, std::memory_order_release);
        sequence.notify_all();
    }
};

struct Broker {
    std::array<Mailbox, 2> mailboxes;
    const bool deferred;
    std::atomic<std::uint64_t> calls{0}, overlaps{0};
    std::atomic<unsigned> active{0};

    explicit Broker(bool hold_delivery = false) noexcept : deferred(hold_delivery) {}
    void deliver(std::uint64_t receipt = 0) noexcept
    {
        for (auto& mailbox : mailboxes) {
            mailbox.receipt.store(receipt, std::memory_order_release);
            mailbox.signal();
        }
    }
    void notify() noexcept
    {
        if (active.fetch_add(1, std::memory_order_acq_rel) != 0) ++overlaps;
        // This host receipt is not a snapshot revision or a PauseEpoch. The
        // notifier neither calls Pause nor waits for a consumer/test gate.
        const auto receipt = calls.fetch_add(1, std::memory_order_release) + 1;
        if (!deferred) deliver(receipt);
        active.fetch_sub(1, std::memory_order_release);
    }
};

struct Counts {
    std::uint64_t notifications = 0, reads = 0, overlapping_calls = 0;
    Counts& operator+=(const Counts& other) noexcept
    {
        notifications += other.notifications;
        reads += other.reads;
        overlapping_calls += other.overlapping_calls;
        return *this;
    }
};

// Each observer owns one thread, so its handlers are serial; the two observers
// may query concurrently. Gates belong to host dispatch, never to the notifier.
struct Observers {
    enum class Window { None, BeforeSnapshot, AfterSnapshot, Quit };
    const PauseSource& source;
    Broker& broker;
    std::barrier<> command{3}, window_entered{3}, window_release{3}, completed{3};
    Window window = Window::None;
    std::array<PauseSnapshot, 2> snapshots{};
    std::array<std::uint64_t, 2> reads{};
    std::array<std::uint64_t, 2> receipts{};
    std::array<std::atomic<unsigned>, 2> handlers{};
    std::array<std::jthread, 2> threads;
    bool closed = false;

    Observers(const PauseSource& value, Broker& delivery) : source(value), broker(delivery)
    {
        for (std::size_t index = 0; index != threads.size(); ++index) {
            threads[index] = std::jthread([&, index] {
                for (;;) {
                    command.arrive_and_wait();
                    if (window == Window::Quit) return;
                    CHECK(handlers[index].fetch_add(1) == 0);
                    // Consume before reading. A concurrent new notification
                    // remains dirty, including one arriving after snapshot().
                    CHECK(broker.mailboxes[index].dirty.exchange(false, std::memory_order_acq_rel));
                    receipts[index] = broker.mailboxes[index].receipt.load(std::memory_order_acquire);
                    if (window == Window::BeforeSnapshot) {
                        window_entered.arrive_and_wait();
                        window_release.arrive_and_wait();
                    }
                    snapshots[index] = source.snapshot();
                    ++reads[index];
                    if (window == Window::AfterSnapshot) {
                        window_entered.arrive_and_wait();
                        window_release.arrive_and_wait();
                    }
                    CHECK(handlers[index].fetch_sub(1) == 1);
                    completed.arrive_and_wait();
                }
            });
        }
    }
    void begin(Window next = Window::None)
    {
        CHECK(!closed);
        window = next;
        command.arrive_and_wait();
    }
    void finish() { completed.arrive_and_wait(); }
    void consume()
    {
        begin();
        finish();
    }
    void expect(const PauseSnapshot& expected) const
    {
        for (const auto& snapshot : snapshots) equal_snapshot(snapshot, expected);
    }
    void close()
    {
        if (closed) return;
        window = Window::Quit;
        command.arrive_and_wait();
        for (auto& thread : threads) thread.join();
        closed = true;
        for (const auto& active : handlers) CHECK(active == 0);
    }
    ~Observers() { close(); }
    Counts counts() const
    {
        CHECK(closed && broker.active == 0);
        return {broker.calls.load(), reads[0] + reads[1], broker.overlaps.load()};
    }
};

Counts coalesced_observers()
{
    Broker broker;
    PauseSource source(AdvanceMode::Continuous, [&]() noexcept { broker.notify(); });
    Observers observers(source, broker);
    {
        auto root = take(source.make_root_branch());
        { auto independent = take(source.make_branch()); }
    }
    const auto final = source.snapshot();
    CHECK(final.revision == 4 && final.phase == GatePhase::Open);
    CHECK(final.live_branch_count == 0 && broker.calls == 4);
    observers.consume();
    observers.expect(final);
    for (const auto& mailbox : broker.mailboxes) CHECK(!mailbox.dirty && mailbox.sequence == 4);
    observers.close();
    CHECK(observers.reads[0] == 1 && observers.reads[1] == 1);
    return observers.counts();
}

Counts reordered_dispatch()
{
    Broker broker(true);
    PauseSource source(AdvanceMode::Continuous, [&]() noexcept { broker.notify(); });
    Observers observers(source, broker);
    {
        auto root = take(source.make_root_branch());
        CHECK(source.request_pause());
    }
    const auto latest = source.snapshot();
    CHECK(latest.revision == 3 && latest.phase == GatePhase::Paused);
    CHECK(latest.live_branch_count == 0 && broker.calls == 3);
    // The host releases held receipts out of order, repeats one, then emits an
    // extra wake. These receipts contain no snapshot and authorize no Resume.
    for (const unsigned receipt : {3u, 1u, 2u, 3u, 0u}) {
        CHECK(receipt <= broker.calls.load());
        broker.deliver(receipt);
        observers.consume();
        for (const auto delivered : observers.receipts) CHECK(delivered == receipt);
        observers.expect(latest);
    }
    CHECK(broker.calls == 3); // Host dispatch did not mutate Pause state.
    observers.close();
    return observers.counts();
}

Counts dirty_window(Observers::Window window)
{
    Broker broker;
    PauseSource source(AdvanceMode::Continuous, [&]() noexcept { broker.notify(); });
    Observers observers(source, broker);
    std::optional<PauseBranch> root(take(source.make_root_branch()));
    const auto before = source.snapshot();
    observers.begin(window);
    observers.window_entered.arrive_and_wait();
    for (const auto& mailbox : broker.mailboxes) CHECK(!mailbox.dirty);
    CHECK(source.request_pause());
    const auto after = source.snapshot();
    CHECK(after.revision == 2 && after.phase == GatePhase::Collecting);
    observers.window_release.arrive_and_wait();
    observers.finish();
    observers.expect(window == Observers::Window::BeforeSnapshot ? after : before);
    for (const auto& mailbox : broker.mailboxes) CHECK(mailbox.dirty);
    observers.consume();
    observers.expect(after);
    for (const auto& mailbox : broker.mailboxes) CHECK(!mailbox.dirty);
    root.reset();
    const auto final = source.snapshot();
    CHECK(final.phase == GatePhase::Paused && final.live_branch_count == 0);
    observers.consume();
    observers.expect(final);
    observers.close();
    return observers.counts();
}

Counts concurrent_notifiers()
{
    Broker broker;
    PauseSource source(AdvanceMode::Continuous, [&]() noexcept { broker.notify(); });
    Observers observers(source, broker);
    std::optional<PauseBranch> root(take(source.make_root_branch()));
    std::barrier launch{5}, created{5}, retire{5};
    std::array<std::jthread, 4> callers;
    for (auto& caller : callers) {
        caller = std::jthread([&] {
            launch.arrive_and_wait();
            auto branch = take(source.make_branch());
            created.arrive_and_wait();
            retire.arrive_and_wait();
        });
    }
    launch.arrive_and_wait();
    created.arrive_and_wait();
    auto expected = source.snapshot();
    CHECK(expected.revision == 5 && expected.live_branch_count == 5 && broker.calls == 5);
    observers.consume();
    observers.expect(expected);
    retire.arrive_and_wait();
    for (auto& caller : callers) caller.join();
    expected = source.snapshot();
    CHECK(expected.revision == 9 && expected.live_branch_count == 1 && broker.calls == 9);
    observers.consume();
    observers.expect(expected);
    root.reset();
    expected = source.snapshot();
    CHECK(expected.revision == 10 && expected.live_branch_count == 0 && broker.calls == 10);
    observers.consume();
    observers.expect(expected);
    observers.close();
    // Overlap is reported, not assumed: barrier launch does not constrain OS
    // scheduling inside a short, nonblocking notifier.
    return observers.counts();
}

Counts shutdown_wakeup_join()
{
    Broker broker;
    PauseSource source(AdvanceMode::Continuous, [&]() noexcept { broker.notify(); });
    std::atomic<bool> quit{false};
    std::atomic<unsigned> borrowed{0}, exited{0};
    std::latch waiting{2}, tail_entered{2}, release_tail{1};
    std::array<PauseSnapshot, 2> last{};
    std::array<std::jthread, 2> observers;
    for (std::size_t index = 0; index != observers.size(); ++index) {
        observers[index] = std::jthread([&, index] {
            auto& mailbox = broker.mailboxes[index];
            const auto observed = mailbox.sequence.load(std::memory_order_acquire);
            ++borrowed;
            waiting.count_down();
            // Both wake-before-wait and wake-after-wait are valid. A reliable
            // sequence makes the quit signal visible in either ordering.
            mailbox.sequence.wait(observed, std::memory_order_acquire);
            CHECK(quit.load(std::memory_order_acquire));
            CHECK(mailbox.dirty.exchange(false, std::memory_order_acq_rel));
            last[index] = source.snapshot();
            tail_entered.count_down();
            release_tail.wait();
            --borrowed;
            ++exited;
        });
    }
    waiting.wait();
    CHECK(borrowed == 2 && exited == 0);
    quit.store(true, std::memory_order_release);
    broker.deliver(); // No Pause mutation is needed to wake idle observers.
    tail_entered.wait();
    CHECK(borrowed == 2 && exited == 0 && broker.calls == 0);
    const auto final = source.snapshot();
    CHECK(final.revision == 0 && final.live_branch_count == 0);
    for (const auto& snapshot : last) equal_snapshot(snapshot, final);
    // Completion/empty counters did not release the host tails. Source and the
    // notification resources remain alive until both actual calls/threads exit.
    release_tail.count_down();
    for (auto& observer : observers) observer.join();
    CHECK(borrowed == 0 && exited == 2);
    return {0, 2, 0};
}

int iterations(int argc, char** argv)
{
    if (argc == 1) return 3;
    CHECK(argc == 3 && std::string_view(argv[1]) == "--iterations");
    const std::string_view text(argv[2]);
    int count = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    CHECK(error == std::errc{} && end == text.data() + text.size());
    CHECK(count > 0 && count <= 1000000);
    return count;
}

template<class Function>
void run_case(const char* id, int count, Function function,
              std::uint64_t notifications_per_scope, std::uint64_t reads_per_scope)
{
    current_case = id;
    Counts total;
    for (int iteration = 0; iteration != count; ++iteration) {
        current_iteration = iteration;
        total += function();
    }
    CHECK(total.notifications == notifications_per_scope * count);
    CHECK(total.reads == reads_per_scope * count);
    std::printf("PASS id=%s complete_scopes=%d notifier_calls=%" PRIu64
                " observer_reads=%" PRIu64 " actual_overlapping_calls=%" PRIu64 "\n",
                id, count, total.notifications, total.reads, total.overlapping_calls);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    const int count = iterations(argc, argv);
    run_case("NOTIFY-01-coalesced", count, coalesced_observers, 4, 2);
    run_case("NOTIFY-02-reordered-extra", count, reordered_dispatch, 3, 10);
    run_case("NOTIFY-03-dirty-before-snapshot", count,
             [] { return dirty_window(Observers::Window::BeforeSnapshot); }, 3, 6);
    run_case("NOTIFY-04-dirty-after-snapshot", count,
             [] { return dirty_window(Observers::Window::AfterSnapshot); }, 3, 6);
    run_case("NOTIFY-05-concurrent-notifiers", count, concurrent_notifiers, 10, 6);
    run_case("NOTIFY-06-shutdown-wakeup-join", count, shutdown_wakeup_join, 0, 2);
    std::printf("real Pause notification matrix: %d complete scopes, 6 stable IDs, %d repetitions each\n",
                6 * count, count);
}
