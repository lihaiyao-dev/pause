/* 测试项目：terminate-composition-probe (历史终止组合探针)
 * 测试项: 场景 1..4 为 body sender error、let 分派抛出、内部 connect 抛出、run_loop 调度后 error;
 * 场景 5..8 为宿主 let_stopped factory 抛出、分派抛出、connect 抛出、unstoppable 调度后 error。
 * 预期: terminate handler 核对启动/分派/调度/撤责/通知次数及业务未收到 error/完成后,
 * 输出 TERMINATE_OK 并以 86 退出; 状态错误为 87, 参数或外逃异常为 88, 未终止为 89。
 * 入口: 同目录 build-run-terminate-probe.cmd 编译并由 run-terminate-probe.ps1 隔离执行 8 个场景。
 * 边界: 不含 pause.hpp, 无真实状态机、callback、任务调度器擦除或侵入式节点。
 * 这是历史组合证据, 不等同于真实 Pause 终止策略的全量验收; 退出 86 还需核对 marker 和 stderr。
 */
#include <stdexec/execution.hpp>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#include <utility>

// Test-only composition probe: no Pause state machine, intrusive nodes, task
// scheduler erasure, callbacks, or production implementation are exercised.
namespace {
struct ProbeFailure {};

struct Control {
    stdexec::run_loop loop;
    stdexec::inplace_stop_source source;
    int scenario = 0;
    int starts = 0;
    int dispatches = 0;
    int host_entries = 0;
    int connects = 0;
    int schedules = 0;
    int schedule_values = 0;
    int injected = 0;
    int body_errors = 0;
    int host_errors = 0;
    int withdrawals = 0;
    int notifications = 0;
    int business_errors = 0;
    int business_completions = 0;
    int after_start = 0;
    bool live = true;
};
Control* active = nullptr;

[[noreturn]] void terminate_checked() noexcept
{
    const auto& c = *active;
    const bool host = c.scenario >= 5;
    const bool scheduled = c.scenario == 4 || c.scenario == 8;
    const bool connecting = c.scenario == 3 || c.scenario == 7;
    const bool valid = c.starts == 1 && c.injected == 1
        && c.dispatches == (c.scenario == 5 ? 0 : 1)
        && c.host_entries == (host ? 1 : 0)
        && c.connects == (connecting ? 1 : 0)
        && c.schedules == (scheduled ? 1 : 0)
        && c.schedule_values == (scheduled ? 1 : 0)
        && c.body_errors == (host ? 0 : 1)
        && c.host_errors == (host ? 1 : 0)
        && c.withdrawals == (host ? 1 : 0)
        && c.notifications == (host ? 1 : 0)
        && c.live == !host
        && c.business_errors == 0 && c.business_completions == 0
        && c.after_start == (scheduled ? 1 : 0)
        && c.source.stop_requested() == host;
    std::fprintf(valid ? stdout : stderr,
        "%s scenario=%d starts=%d dispatches=%d host_entries=%d connects=%d "
        "schedules=%d schedule_values=%d injected=%d body_errors=%d host_errors=%d "
        "withdrawals=%d notifications=%d live=%d business_errors=%d "
        "business_completions=%d after_start=%d token_stopped=%d\n",
        valid ? "TERMINATE_OK" : "TERMINATE_BAD_STATE", c.scenario, c.starts,
        c.dispatches, c.host_entries, c.connects, c.schedules, c.schedule_values,
        c.injected, c.body_errors, c.host_errors, c.withdrawals, c.notifications,
        c.live, c.business_errors, c.business_completions, c.after_start,
        c.source.stop_requested());
    std::fflush(valid ? stdout : stderr);
    std::_Exit(valid ? 86 : 87);
}

template<class Inner, bool Accounting>
struct TerminateSender {
    using sender_concept = stdexec::sender_tag;
    Inner inner;
    Control* control;

    auto get_env() const noexcept { return stdexec::get_env(inner); }
    template<class, class Env>
    static consteval auto get_completion_signatures() noexcept
    {
        return stdexec::completion_signatures_of_t<Inner, Env>{};
    }
    template<class Receiver>
    struct Target {
        using receiver_concept = stdexec::receiver_tag;
        Receiver receiver;
        Control* control;
        decltype(auto) get_env() const noexcept { return stdexec::get_env(receiver); }
        template<class... Values>
        void set_value(Values&&... values) noexcept
        {
            stdexec::set_value(std::move(receiver), static_cast<Values&&>(values)...);
        }
        void set_stopped() noexcept
        {
            if constexpr (Accounting) {
                if (control->live) {
                    control->live = false;
                    ++control->withdrawals;
                    ++control->notifications;
                }
            }
            stdexec::set_stopped(std::move(receiver));
        }
        template<class Error>
        [[noreturn]] void set_error(Error&&) noexcept
        {
            if constexpr (Accounting) ++control->body_errors;
            else ++control->host_errors;
            std::terminate();
        }
    };
    template<class Receiver>
    struct Operation {
        stdexec::connect_result_t<Inner, Target<Receiver>> inner;
        Operation(Inner sender, Receiver receiver, Control* control)
            : inner(stdexec::connect(std::move(sender),
                    Target<Receiver>{std::move(receiver), control})) {}
        void start() & noexcept { stdexec::start(inner); }
    };
    template<class Receiver>
    auto connect(Receiver receiver) &&
    {
        return Operation<Receiver>{std::move(inner), std::move(receiver), control};
    }
};

template<bool Accounting, class Inner>
auto terminate_errors(Inner sender, Control& control)
{
    return TerminateSender<Inner, Accounting>{std::move(sender), &control};
}

// This intentionally throwing *test sender* is connected by real let_value
// after the public operation starts. No task_scheduler backend is involved.
struct ThrowConnectSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t()>;
    Control* control;
    template<class Receiver>
    struct Operation {
        Receiver receiver;
        void start() & noexcept { stdexec::set_value(std::move(receiver)); }
    };
    template<class Receiver>
    auto connect(Receiver) const -> Operation<Receiver>
    {
        ++control->connects;
        ++control->injected;
        throw ProbeFailure{};
    }
};

struct RootReceiver {
    using receiver_concept = stdexec::receiver_tag;
    Control* control;
    auto get_env() const noexcept
    {
        return stdexec::env{
            stdexec::prop{stdexec::get_start_scheduler, control->loop.get_scheduler()},
            stdexec::prop{stdexec::get_stop_token, control->source.get_token()}};
    }
    void set_value() noexcept
    {
        ++control->business_completions;
        control->loop.finish();
    }
    void set_stopped() noexcept
    {
        ++control->business_completions;
        control->loop.finish();
    }
    template<class Error>
    void set_error(Error&&) noexcept
    {
        ++control->business_completions;
        control->loop.finish();
    }
};

template<class Sender>
void run_to_death(Sender sender, Control& control)
{
    auto guarded = stdexec::let_error(std::move(sender), [&control](auto&&) {
        ++control.business_errors;
        return stdexec::just();
    });
    auto operation = stdexec::connect(std::move(guarded), RootReceiver{&control});
    ++control.starts;
    stdexec::start(operation);
    ++control.after_start;
    control.loop.run();
    std::fprintf(stderr, "FAIL: operation survived expected termination\n");
}

template<int Kind>
auto failure_route(Control& control)
{
    return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
        [&control](auto scheduler) {
            ++control.dispatches;
            if constexpr (Kind == 1) {
                ++control.injected;
                return stdexec::just_error(std::make_exception_ptr(ProbeFailure{}));
            } else if constexpr (Kind == 2) {
                ++control.injected;
                throw ProbeFailure{};
                return stdexec::just();
            } else if constexpr (Kind == 3) {
                return ThrowConnectSender{&control};
            } else {
                ++control.schedules;
                // Concrete run_loop connect is noexcept. The injected error is
                // after that actual operation delivers value, never an erased
                // backend allocation/connect path with unreachable behavior.
                if constexpr (Kind == 4) {
                    return stdexec::let_value(stdexec::schedule(scheduler), [&control] {
                        ++control.schedule_values;
                        ++control.injected;
                        return stdexec::just_error(std::make_exception_ptr(ProbeFailure{}));
                    });
                } else {
                    return stdexec::let_value(stdexec::unstoppable(stdexec::schedule(scheduler)),
                        [&control] {
                            ++control.schedule_values;
                            ++control.injected;
                            return stdexec::just_error(std::make_exception_ptr(ProbeFailure{}));
                        });
                }
            }
        });
}

template<int Kind>
void body_case(Control& control)
{
    run_to_death(terminate_errors<true>(failure_route<Kind>(control), control), control);
}

template<int Kind>
void host_case(Control& control)
{
    (void) control.source.request_stop();
    auto body = terminate_errors<true>(stdexec::just_stopped(), control);
    auto adapted = stdexec::let_stopped(std::move(body), [&control] {
        ++control.host_entries;
        if constexpr (Kind == 0) {
            ++control.injected;
            throw ProbeFailure{};
            return stdexec::just_stopped();
        } else {
            return failure_route<Kind>(control);
        }
    });
    run_to_death(terminate_errors<false>(std::move(adapted), control), control);
}
} // namespace

int main(int argc, char** argv)
{
    Control control;
    active = &control;
    std::set_terminate(terminate_checked);
    try {
        if (argc != 2) return 88;
        control.scenario = std::stoi(argv[1]);
        switch (control.scenario) {
        case 1: body_case<1>(control); break;
        case 2: body_case<2>(control); break;
        case 3: body_case<3>(control); break;
        case 4: body_case<4>(control); break;
        case 5: host_case<0>(control); break;
        case 6: host_case<2>(control); break;
        case 7: host_case<3>(control); break;
        case 8: host_case<5>(control); break;
        default: return 88;
        }
    } catch (...) {
        std::fprintf(stderr, "FAIL: exception escaped composition\n");
        return 88;
    }
    return 89;
}
