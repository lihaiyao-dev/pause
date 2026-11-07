/* 测试项目：pause_outer_lifetime_test
 * 目的: 验证真实 Pause 与外层 sender 的非值完成, operation 销毁及宿主调用尾部边界.
 * 测试项:
 * - 4 个普通项: 暂停点在 Open 同步 value 或恢复后 value, 外层随后返回
 *   stopped/error; 外层终止和 operation 销毁均不得提前撤销借用分支的责任.
 * - 2 个线程项: .affine() 的宿主停止转发仍处于 schedule.start/request_stop
 *   调用尾部时, 分别由完成接收者或协调线程销毁外层 operation.
 * 关键预期: 外层只完成一次, 完成线程匹配; 停止路径只调度一次, 撤销等待责任
 * 保留旧 epoch; operation 可已销毁, 但 Source/stop source/宿主状态必须保留
 * 到请求尾部返回并 join 工作线程; 普通项保留分支直到显式退役才收敛暂停.
 * 入口: PAUSE_BUILD_TESTS=ON 时注册同名 CTest.
 *   ctest --test-dir <build> -C <config> -R "^pause_outer_lifetime_test$" --output-on-failure
 *   pause_outer_lifetime_test --iterations 3 --ordinary-iterations 3
 * 参数: 两个次数均为 1..1000000; iterations 默认 3, ordinary-iterations 默认
 * 跟随 iterations, 分别控制 2 个线程项和 4 个普通项.
 * 边界: 透明 run_loop 外壳只控制宿主 start 尾部, 通过显式 .affine() 使用,
 * 不经过 task_scheduler 擦除; 不阻塞 notifier, 不执行悬空 Source 或数据竞争.
 */
#include "../support/pause_test_notifications.hpp"
#include "pause_sender_test_support.hpp"

#include <optional>

// A transparent run_loop facade gates only the host schedule's initiating
// start tail. It is used by explicit .affine(), never task_scheduler erasure.
// The notifier remains nonblocking. All tail coordination lives outside ops.
namespace {
using namespace pause_sender_test;
using coop_pause::AdvanceMode;
using coop_pause::GatePhase;
using coop_pause::PauseBranch;
using coop_pause::PauseSource;

struct TailControl {
    stdexec::run_loop loop;
    stdexec::inplace_stop_source stop;
    PauseSource* source = nullptr;
    Gate schedule_tail;
    std::latch waiting{1}, completed{1};
    std::atomic<int> schedules{0}, starts{0}, completions{0};
    std::atomic<int> destroyed{0}, tail_returned{0}, request_returned{0};
    std::thread::id worker_id, completion_id;
    bool destroy_in_completion = false;
    void* outer = nullptr;
    void (*destroy_outer)(TailControl*) noexcept = nullptr;
};

struct TailScheduleSender;
struct TailScheduler {
    using scheduler_concept = stdexec::scheduler_tag;
    TailControl* control;
    bool operator==(const TailScheduler&) const noexcept = default;
    auto query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
    { return *this; }
    auto schedule() const noexcept -> TailScheduleSender;
};

struct TailScheduleSender {
    using sender_concept = stdexec::sender_tag;
    using Base = stdexec::schedule_result_t<stdexec::run_loop::scheduler>;
    TailControl* control;
    struct Attrs {
        TailControl* control;
        template<class Tag>
            requires (std::same_as<Tag, stdexec::set_value_t>
                      || std::same_as<Tag, stdexec::set_stopped_t>)
        auto query(stdexec::get_completion_scheduler_t<Tag>) const noexcept
        { return TailScheduler{control}; }
    };
    auto get_env() const noexcept { return Attrs{control}; }
    template<class, class Env>
    static consteval auto get_completion_signatures() noexcept
    { return stdexec::completion_signatures_of_t<Base, Env>{}; }
    template<class Receiver>
    struct Operation {
        TailControl* control;
        stdexec::connect_result_t<Base, Receiver> inner;
        Operation(TailControl* current, Receiver receiver)
            : control(current), inner(stdexec::connect(
                  stdexec::schedule(current->loop.get_scheduler()), std::move(receiver))) {}
        void start() & noexcept
        {
            auto* local = control;
            ++local->starts;
            stdexec::start(inner);
            // inner/this may already be destroyed by the run_loop thread.
            // Only external state saved before start may be touched below.
            local->schedule_tail.hold();
            CHECK(local->destroyed == 1 && local->request_returned == 0);
            ++local->tail_returned;
        }
    };
    template<class Receiver>
    auto connect(Receiver receiver) const
    {
        const auto token = stdexec::get_stop_token(stdexec::get_env(receiver));
        // This schedule is the host hop after the real Pause callback stopped.
        // Its original receiver token is stopped; only this hop is unstoppable.
        CHECK(!token.stop_possible());
        CHECK(control->stop.stop_requested());
        return Operation<Receiver>{control, std::move(receiver)};
    }
};
auto TailScheduler::schedule() const noexcept -> TailScheduleSender
{
    ++control->schedules;
    return {control};
}
static_assert(stdexec::scheduler<TailScheduler>);

struct TailReceiver {
    using receiver_concept = stdexec::receiver_tag;
    TailControl* control;
    auto get_env() const noexcept
    {
        return stdexec::env{
            stdexec::prop{stdexec::get_stop_token, control->stop.get_token()},
            stdexec::prop{stdexec::get_start_scheduler, TailScheduler{control}}};
    }
    void set_value() noexcept { CHECK(false); }
    template<class Error> void set_error(Error&&) noexcept { CHECK(false); }
    void set_stopped() noexcept
    {
        auto* local = control;
        CHECK(local->source->snapshot().live_branch_count == 0);
        CHECK(local->request_returned == 0 && local->tail_returned == 0);
        CHECK(++local->completions == 1);
        local->completion_id = std::this_thread::get_id();
        if (local->destroy_in_completion) local->destroy_outer(local);
        local->completed.count_down();
        // Completion can delete this receiver; no member access after the CPO.
    }
};

void host_stop_inflight(bool destroy_in_completion)
{
    TailControl control;
    std::atomic<int> notifications{0};
    PauseSource source(AdvanceMode::SingleStep, pause_test::notifications([&]() noexcept { ++notifications; }));
    control.source = &source;
    control.destroy_in_completion = destroy_in_completion;
    std::optional<PauseBranch> root;
    {
        auto branch = source.make_root_branch(); CHECK(branch);
        root.emplace(std::move(*branch));
    }
    using Sender = decltype(root->suspend_point().affine());
    using Operation = stdexec::connect_result_t<Sender, TailReceiver>;
    control.outer = new Operation(stdexec::connect(root->suspend_point().affine(), TailReceiver{&control}));
    control.destroy_outer = [](TailControl* local) noexcept {
        auto* operation = static_cast<Operation*>(std::exchange(local->outer, nullptr));
        CHECK(operation != nullptr);
        delete operation;
        CHECK(++local->destroyed == 1);
    };
    auto entry = stdexec::connect(stdexec::then(stdexec::schedule(control.loop.get_scheduler()), [&] {
        stdexec::start(*static_cast<Operation*>(control.outer));
        control.waiting.count_down();
    }), IgnoreReceiver{});
    std::jthread worker([&] {
        control.worker_id = std::this_thread::get_id();
        control.loop.run();
    });
    stdexec::start(entry);
    control.waiting.wait();
    const auto paused = source.snapshot();
    CHECK(paused.phase == GatePhase::Paused && paused.waiting_branch_count == 1);
    std::jthread requester([&] {
        CHECK(control.stop.request_stop());
        ++control.request_returned;
    });
    control.schedule_tail.entered.wait();
    control.completed.wait();
    if (!destroy_in_completion) control.destroy_outer(&control);
    // let_stopped has replaced the completed Pause body before starting the
    // host schedule. Its callback was self-unregistered on the request thread;
    // outer destruction therefore does not wait on that still-held start tail.
    CHECK(control.destroyed == 1 && control.outer == nullptr);
    CHECK(control.request_returned == 0 && control.tail_returned == 0);
    CHECK(control.completions == 1 && control.completion_id == control.worker_id);
    CHECK(control.schedules == 1 && control.starts == 1);
    const auto withdrawn = source.snapshot();
    CHECK(withdrawn.phase == GatePhase::Paused && withdrawn.paused_epoch == paused.paused_epoch);
    CHECK(withdrawn.live_branch_count == 0 && withdrawn.waiting_branch_count == 0);
    CHECK(withdrawn.revision == paused.revision + 1 && notifications == 2);
    // Empty counters/outer destruction do not shorten source or stop-source
    // lifetime: the initiating request stack must actually leave the gate.
    control.schedule_tail.release.count_down();
    requester.join();
    control.loop.finish();
    worker.join();
    CHECK(control.request_returned == 1 && control.tail_returned == 1);
    root.reset();
    CHECK(source.snapshot().live_branch_count == 0 && notifications == 2);
}

enum class OuterResult { None, Stopped, Error };
struct BoundaryControl {
    SchedulerState scheduler;
    stdexec::inplace_stop_source stop;
    PauseSource* source = nullptr;
    OuterResult result = OuterResult::None;
    int pause_values = 0, completions = 0;
};
struct BoundaryReceiver {
    using receiver_concept = stdexec::receiver_tag;
    BoundaryControl* control;
    auto get_env() const noexcept
    {
        return stdexec::env{
            stdexec::prop{stdexec::get_stop_token, control->stop.get_token()},
            stdexec::prop{stdexec::get_start_scheduler, CountingScheduler{&control->scheduler}}};
    }
    void set_value() noexcept { CHECK(false); }
    void finish(OuterResult outcome) noexcept
    {
        CHECK(control->pause_values == 1 && ++control->completions == 1);
        CHECK(control->source->snapshot().live_branch_count == 1);
        control->result = outcome;
        control->scheduler.loop.finish();
    }
    void set_stopped() noexcept { finish(OuterResult::Stopped); }
    void set_error(int error) noexcept { CHECK(error == 42); finish(OuterResult::Error); }
    template<class Error> void set_error(Error&&) noexcept { CHECK(false); }
};

template<bool Error>
void outer_after_value(bool resumed)
{
    BoundaryControl control;
    PauseSource source(AdvanceMode::Continuous);
    control.source = &source;
    std::optional<PauseBranch> root;
    {
        auto branch = source.make_root_branch(); CHECK(branch);
        root.emplace(std::move(*branch));
    }
    if (resumed) CHECK(source.request_pause());
    {
        auto outer = stdexec::let_value(root->suspend_point(), [&] {
            ++control.pause_values;
            CHECK(source.snapshot().live_branch_count == 1);
            if constexpr (Error) return stdexec::just_error(42);
            else return stdexec::just_stopped();
        });
        auto operation = stdexec::connect(std::move(outer), BoundaryReceiver{&control});
        run_in_context(control.scheduler, [&] {
            stdexec::start(operation);
            if (resumed) {
                CHECK(control.result == OuterResult::None);
                const auto paused = source.snapshot();
                CHECK(paused.phase == GatePhase::Paused && paused.waiting_branch_count == 1);
                CHECK(source.resume(*paused.paused_epoch, AdvanceMode::Continuous));
                CHECK(control.result == OuterResult::None);
            }
        });
        CHECK(control.result == (Error ? OuterResult::Error : OuterResult::Stopped));
        CHECK(control.pause_values == 1 && control.completions == 1);
        CHECK(control.scheduler.calls == (resumed ? 1 : 0));
        CHECK(control.scheduler.starts == (resumed ? 1 : 0));
        CHECK(source.snapshot().phase == GatePhase::Open);
        CHECK(source.snapshot().live_branch_count == 1);
    }
    // Neither outer non-value completion nor operation destruction owns this
    // borrowed branch. Its retained responsibility prevents pause convergence.
    CHECK(source.request_pause());
    CHECK(source.snapshot().phase == GatePhase::Collecting);
    CHECK(source.snapshot().live_branch_count == 1);
    root.reset();
    const auto final = source.snapshot();
    CHECK(final.phase == GatePhase::Paused && final.paused_epoch);
    CHECK(final.live_branch_count == 0 && final.waiting_branch_count == 0);
}

template<class Function>
void run_case(const char* id, int count, Function function)
{
    for (int iteration = 0; iteration != count; ++iteration) function();
    std::printf("PASS case=%s complete_scopes=%d\n", id, count);
    std::fflush(stdout);
}
} // namespace

int main(int argc, char** argv)
{
    int count = 3, ordinary = 0;
    CHECK(argc % 2 == 1);
    for (int index = 1; index < argc; index += 2) {
        const std::string_view option(argv[index]), value(argv[index + 1]);
        int parsed = 0;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
        CHECK(error == std::errc{} && end == value.data() + value.size() && parsed > 0 && parsed <= 1000000);
        if (option == "--iterations") count = parsed;
        else { CHECK(option == "--ordinary-iterations"); ordinary = parsed; }
    }
    if (ordinary == 0) ordinary = count;
    run_case("outer.open.stopped", ordinary, [] { outer_after_value<false>(false); });
    run_case("outer.open.error", ordinary, [] { outer_after_value<true>(false); });
    run_case("outer.resumed.stopped", ordinary, [] { outer_after_value<false>(true); });
    run_case("outer.resumed.error", ordinary, [] { outer_after_value<true>(true); });
    run_case("host-stop.inflight.receiver-destroy", count, [] { host_stop_inflight(true); });
    run_case("host-stop.inflight.coordinator-destroy", count, [] { host_stop_inflight(false); });
    std::printf("real Pause outer lifetime: %d complete scopes, 4 ordinary x %d, 2 threaded x %d\n",
                ordinary * 4 + count * 2, ordinary, count);
}
