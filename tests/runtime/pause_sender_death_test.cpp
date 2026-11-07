/* 测试项目：pause_sender_death_test
 * 性质: 真实 Pause 内部调度故障的子进程终止测试, 不在主测试进程触发 terminate.
 * 测试项目:
 * - body 路径: Paused 恢复时分别注入 schedule 抛异常、connect 抛异常、set_error 完成.
 * - affine 路径: 取消触发亲和补跳时分别注入同样三类故障.
 * - 每例必须到达指定注入点后调用 terminate handler 并退出 77; 提前终止或普通完成均失败.
 * - 基础设施失败不得转为对外 value/stopped/error, 不得到达外层 let_error 业务恢复,
 *   进入失败调度后也不得再触发 Pause 清理通知.
 * 运行: ctest --test-dir <build> -R '^pause_sender_death_test$' --output-on-failure.
 * CLI: --iterations N (默认 1, 范围 1..1000000), 每轮启动 6 个子进程.
 * 内部入口: --child body-schedule|body-connect|body-error|affine-schedule|affine-connect|affine-error.
 * 边界: 使用行为明确的故障 scheduler, 不经过 task_scheduler 类型擦除,
 *   不执行 stdexec 不支持的擦除后分配/connect 失败路径; 非指定退出码绝不计为通过.
 */
#include "../support/pause_test_notifications.hpp"
#include "pause_sender_test_support.hpp"

#include <exception>
#include <stdexcept>
#if defined(_WIN32)
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace pause_sender_test;
enum class Failure { Schedule, Connect, Error };
enum class FaultPoint { None, ScheduleThrow, ConnectThrow, ErrorDelivery };
struct FaultState {
    stdexec::run_loop loop;
    Failure failure = Failure::Error;
    bool armed = false;
    FaultPoint reached = FaultPoint::None;
    std::atomic<bool>* failing = nullptr;
};
thread_local FaultState* expected_fault = nullptr;
struct FaultSender;
struct FaultScheduler {
    using scheduler_concept = stdexec::scheduler_tag;
    FaultState* state;
    bool operator==(const FaultScheduler&) const noexcept = default;
    auto query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
    { return *this; }
    auto schedule() const -> FaultSender;
};
struct FaultSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(),
        stdexec::set_stopped_t(), stdexec::set_error_t(std::exception_ptr)>;
    FaultState* state;
    struct Attrs {
        FaultState* state;
        template<class Tag>
            requires (std::same_as<Tag, stdexec::set_value_t> || std::same_as<Tag, stdexec::set_stopped_t>)
        auto query(stdexec::get_completion_scheduler_t<Tag>) const noexcept
        { return FaultScheduler{state}; }
    };
    auto get_env() const noexcept { return Attrs{state}; }
    template<class R>
    struct Operation {
        FaultState* state;
        R receiver;
        void start() & noexcept {
            CHECK(state->armed && state->failure == Failure::Error);
            auto error = std::make_exception_ptr(std::runtime_error("schedule error"));
            state->reached = FaultPoint::ErrorDelivery;
            stdexec::set_error(std::move(receiver), std::move(error));
        }
    };
    template<class R>
    auto connect(R receiver) const {
        if (state->armed && state->failure == Failure::Connect) {
            const std::runtime_error error("schedule connect failure");
            state->reached = FaultPoint::ConnectThrow;
            throw error;
        }
        return Operation<R>{state, std::move(receiver)};
    }
};
auto FaultScheduler::schedule() const -> FaultSender {
    if (state->armed) *state->failing = true;
    if (state->armed && state->failure == Failure::Schedule) {
        const std::runtime_error error("schedule factory failure");
        state->reached = FaultPoint::ScheduleThrow;
        throw error;
    }
    return {state};
}
static_assert(stdexec::scheduler<FaultScheduler>);

struct FaultReceiver {
    using receiver_concept = stdexec::receiver_tag;
    FaultState* state;
    stdexec::inplace_stop_token token;
    auto get_env() const noexcept {
        return stdexec::env{stdexec::prop{stdexec::get_stop_token, token},
                            stdexec::prop{stdexec::get_start_scheduler, FaultScheduler{state}}};
    }
    void set_value() noexcept { std::_Exit(91); }
    void set_stopped() noexcept { std::_Exit(92); }
    template<class E> void set_error(E&&) noexcept { std::_Exit(93); }
};

// Fault backends are used only without task_scheduler erasure. Their failure
// behavior is explicit and defined; this never executes stdexec's unsupported
// erased-backend allocation/connect-unreachable path.
template<bool Affine>
void child(Failure failure)
{
    FaultState state; state.failure = failure;
    stdexec::inplace_stop_source stop;
    std::atomic<bool> failing{false};
    state.failing = &failing;
    coop_pause::PauseSource source(coop_pause::AdvanceMode::SingleStep, pause_test::notifications([&]() noexcept {
        if (failing) std::_Exit(94); // No cleanup/notification after internal error.
    }));
    auto root = source.make_root_branch(); CHECK(root);
    auto sender = [&] {
        if constexpr (Affine) return root->suspend_point().affine();
        else return root->suspend_point();
    }();
    auto unrecoverable = stdexec::let_error(std::move(sender), [](auto&&) {
        std::_Exit(95); // Outer business recovery must remain unreachable.
        return stdexec::just();
    });
    auto operation = stdexec::connect(std::move(unrecoverable), FaultReceiver{&state, stop.get_token()});
    expected_fault = &state;
    std::set_terminate([] {
        const auto* fault = expected_fault;
        if (!fault || !fault->armed || !fault->failing->load()) std::_Exit(78);
        const auto expected = fault->failure == Failure::Schedule ? FaultPoint::ScheduleThrow
                            : fault->failure == Failure::Connect ? FaultPoint::ConnectThrow
                            : FaultPoint::ErrorDelivery;
        // A terminate before the designated injection is a regression, not a
        // successful death test (for example, an unrelated start invariant).
        std::_Exit(fault->reached == expected ? 77 : 79);
    });
    // Host launch runs in the actual queue context. Its receiver advertises the
    // transparent fault facade solely to inject later infrastructure failures.
    auto entry = stdexec::connect(stdexec::then(stdexec::schedule(state.loop.get_scheduler()), [&] {
        stdexec::start(operation);
        state.armed = true;
        if constexpr (Affine) {
            // Legitimate stopped accounting occurs before the host failure;
            // only once the host scheduler is entered is further notification forbidden.
            CHECK(stop.request_stop());
        } else {
            CHECK(source.resume(*source.snapshot().paused_epoch, coop_pause::AdvanceMode::Continuous));
        }
        std::_Exit(96);
    }), IgnoreReceiver{});
    stdexec::start(entry);
    state.loop.run();
    std::_Exit(97);
}

int launch(const char* executable, const char* test_case)
{
#if defined(_WIN32)
    const char* arguments[]{executable, "--child", test_case, nullptr};
    return static_cast<int>(_spawnv(_P_WAIT, executable, arguments));
#else
    const auto pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) { execl(executable, executable, "--child", test_case, nullptr); std::_Exit(98); }
    int status = 0;
    CHECK(waitpid(pid, &status, 0) == pid);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}
} // namespace

int main(int argc, char** argv) {
    constexpr const char* cases[]{"body-schedule", "body-connect", "body-error",
                                 "affine-schedule", "affine-connect", "affine-error"};
    if (argc == 3 && std::string_view(argv[1]) == "--child") {
        const std::string_view mode(argv[2]);
        bool known = false;
        for (const char* test_case : cases) known |= mode == test_case;
        CHECK(known);
        const Failure failure = mode.ends_with("schedule") ? Failure::Schedule
                              : mode.ends_with("connect") ? Failure::Connect : Failure::Error;
        if (mode.starts_with("affine-")) child<true>(failure);
        else child<false>(failure);
    }
    const int count = argc == 1 ? 1 : iterations(argc, argv);
    for (const char* test_case : cases) {
        for (int iteration = 0; iteration != count; ++iteration) {
            const int code = launch(argv[0], test_case);
            if (code != 77) std::fprintf(stderr, "%s iteration %d exited %d, expected terminate handler 77\n",
                                        test_case, iteration, code);
            CHECK(code == 77);
        }
        std::printf("PASS termination=%s subprocesses=%d expected_exit=77\n", test_case, count);
        std::fflush(stdout);
    }
    std::printf("PASS: %d real Pause infrastructure termination subprocesses; outer let_error never runs\n", 6 * count);
}
