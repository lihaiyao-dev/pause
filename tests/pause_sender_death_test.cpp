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
struct FaultState {
    stdexec::run_loop loop;
    Failure failure = Failure::Error;
    bool armed = false;
    std::atomic<bool>* failing = nullptr;
};
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
            stdexec::set_error(std::move(receiver), std::make_exception_ptr(std::runtime_error("schedule error")));
        }
    };
    template<class R>
    auto connect(R receiver) const {
        if (state->armed && state->failure == Failure::Connect)
            throw std::runtime_error("schedule connect failure");
        return Operation<R>{state, std::move(receiver)};
    }
};
auto FaultScheduler::schedule() const -> FaultSender {
    if (state->armed) *state->failing = true;
    if (state->armed && state->failure == Failure::Schedule)
        throw std::runtime_error("schedule factory failure");
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
    pause::PauseSource source(pause::AdvanceMode::SingleStep, [&]() noexcept {
        if (failing) std::_Exit(94); // No cleanup/notification after internal error.
    });
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
    std::set_terminate([] { std::_Exit(77); });
    // Host launch runs in the actual queue context. Its receiver advertises the
    // transparent fault facade solely to inject later infrastructure failures.
    auto entry = stdexec::connect(stdexec::then(stdexec::schedule(state.loop.get_scheduler()), [&] {
        stdexec::start(operation);
        state.armed = true;
        if constexpr (Affine) {
            // Legitimate stopped accounting occurs before the host failure;
            // only once the host scheduler is entered is further notification forbidden.
            CHECK(!stop.request_stop());
        } else {
            CHECK(source.resume(*source.snapshot().paused_epoch, pause::AdvanceMode::Continuous));
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
    if (argc == 3 && std::string_view(argv[1]) == "--child") {
        const std::string_view mode(argv[2]);
        const Failure failure = mode.ends_with("schedule") ? Failure::Schedule
                              : mode.ends_with("connect") ? Failure::Connect : Failure::Error;
        if (mode.starts_with("affine-")) child<true>(failure);
        else child<false>(failure);
    }
    CHECK(argc == 1);
    for (const char* test_case : {"body-schedule", "body-connect", "body-error",
                                 "affine-schedule", "affine-connect", "affine-error"}) {
        const int code = launch(argv[0], test_case);
        if (code != 77) std::fprintf(stderr, "%s exited %d, expected terminate handler 77\n", test_case, code);
        CHECK(code == 77);
    }
    std::puts("PASS: 6 real Pause infrastructure termination subprocesses; outer let_error never runs");
}
