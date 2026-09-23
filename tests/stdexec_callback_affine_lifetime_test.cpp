#include <stdexec/execution.hpp>

#include <atomic>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

// A pinned-dependency composition probe, not a Pause implementation. The real
// let_stopped switches away from an operation whose real stop callback is still
// executing, then queues an unstoppable run_loop hop before delivering stopped.
namespace {

[[noreturn]] void fail(const char* expression, int line) noexcept
{
    std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

struct Control {
    stdexec::run_loop loop;
    stdexec::inplace_stop_source source;
    std::atomic<int> starts{0};
    std::atomic<int> callbacks{0};
    std::atomic<int> raw_destroyed{0};
    std::atomic<int> raw_destroyed_in_callback{0};
    std::atomic<int> after_terminal{0};
    std::atomic<int> callback_returning{0};
    std::atomic<int> request_returned{0};
    std::atomic<int> schedules{0};
    std::atomic<int> schedule_values{0};
    std::atomic<int> stopped{0};
    std::latch terminal_returned{1};
    std::latch release_callback_tail{1};
    std::latch completed{1};
    bool hold_callback_tail = false;
    std::thread::id callback_thread;
    std::thread::id raw_destruction_thread;
    std::thread::id scheduler_thread;
    std::thread::id completion_thread;
};

struct CallbackSender {
    using sender_concept = stdexec::sender_tag;
    using completion_signatures = stdexec::completion_signatures<stdexec::set_stopped_t()>;
    Control* control;

    template<class Receiver>
    struct Operation {
        struct StopFn {
            Operation* operation;
            void operator()() noexcept { operation->on_stop(); }
        };

        Control* control;
        Receiver receiver;
        std::optional<stdexec::inplace_stop_callback<StopFn>> callback;

        Operation(Control* current, Receiver target)
            : control(current), receiver(std::move(target)) {}
        Operation(const Operation&) = delete;
        Operation(Operation&&) = delete;

        ~Operation()
        {
            // Finish unregistering before recording destruction. In the tested
            // path this is same-thread self-unregistration inside set_stopped.
            auto* local = control;
            callback.reset();
            local->raw_destruction_thread = std::this_thread::get_id();
            if (local->callbacks == 1 && local->after_terminal == 0)
                ++local->raw_destroyed_in_callback;
            ++local->raw_destroyed;
        }

        void start() & noexcept
        {
            auto token = stdexec::get_stop_token(stdexec::get_env(receiver));
            static_assert(std::is_same_v<decltype(token), stdexec::inplace_stop_token>);
            CHECK(token == control->source.get_token());
            // This probe deliberately requests stop only after start returns;
            // construction-phase cancellation has its separate hook tests.
            CHECK(!token.stop_requested());
            ++control->starts;
            callback.emplace(token, StopFn{this});
        }

        void on_stop() noexcept
        {
            auto* local = control;
            local->callback_thread = std::this_thread::get_id();
            ++local->callbacks;
            stdexec::set_stopped(std::move(receiver));
            // The terminal CPO has destroyed this operation and StopFn through
            // let_stopped's predecessor replacement. Only externally owned
            // test coordination state may be touched below.
            CHECK(local->raw_destroyed == 1);
            ++local->after_terminal;
            local->terminal_returned.count_down();
            if (local->hold_callback_tail) local->release_callback_tail.wait();
            ++local->callback_returning;
        }
    };

    template<class Receiver>
    auto connect(Receiver receiver) const
    {
        return Operation<Receiver>{control, std::move(receiver)};
    }
};

struct RootReceiver {
    using receiver_concept = stdexec::receiver_tag;
    Control* control;

    auto get_env() const noexcept
    {
        return stdexec::env{
            stdexec::prop{stdexec::get_stop_token, control->source.get_token()},
            stdexec::prop{stdexec::get_start_scheduler, control->loop.get_scheduler()}};
    }

    void set_stopped() noexcept
    {
        auto* local = control;
        CHECK(local->raw_destroyed == 1);
        CHECK(local->schedule_values == 1);
        CHECK(++local->stopped == 1);
        local->completion_thread = std::this_thread::get_id();
        local->loop.finish();
        local->completed.count_down();
        // The coordinator may now destroy this receiver and its outer operation.
    }

    template<class Error>
    void set_error(Error&&) noexcept { CHECK(false); }
};

auto make_sender(Control& control)
{
    return stdexec::let_stopped(CallbackSender{&control}, [local = &control] {
        return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
            [local](auto scheduler) {
                ++local->schedules;
                return stdexec::let_value(stdexec::unstoppable(stdexec::schedule(scheduler)),
                    [local] {
                        // Proves that the host schedule delivered value despite
                        // the original token already being stopped.
                        CHECK(local->source.stop_requested());
                        ++local->schedule_values;
                        return stdexec::just_stopped();
                    });
            });
    });
}

void check_finished(const Control& control)
{
    CHECK(control.starts == 1 && control.callbacks == 1);
    CHECK(control.raw_destroyed == 1 && control.raw_destroyed_in_callback == 1);
    CHECK(control.after_terminal == 1 && control.callback_returning == 1);
    CHECK(control.request_returned == 1);
    CHECK(control.schedules == 1 && control.schedule_values == 1);
    CHECK(control.stopped == 1);
    CHECK(control.raw_destruction_thread == control.callback_thread);
    CHECK(control.completion_thread == control.scheduler_thread);
}

void callback_returns_before_host_runs()
{
    Control control;
    auto operation = stdexec::connect(make_sender(control), RootReceiver{&control});
    stdexec::start(operation);
    CHECK(control.starts == 1 && control.raw_destroyed == 0);
    CHECK(!control.source.request_stop());
    ++control.request_returned;
    CHECK(control.raw_destroyed == 1 && control.callback_returning == 1);
    CHECK(control.schedules == 1 && control.schedule_values == 0 && control.stopped == 0);
    control.scheduler_thread = std::this_thread::get_id();
    control.loop.run();
    check_finished(control);
}

void outer_destroyed_before_callback_returns()
{
    Control control;
    control.hold_callback_tail = true;
    using OuterOperation = decltype(stdexec::connect(make_sender(control), RootReceiver{&control}));
    std::unique_ptr<OuterOperation> operation{
        new OuterOperation(stdexec::connect(make_sender(control), RootReceiver{&control}))};
    stdexec::start(*operation);
    std::thread requester([&] {
        CHECK(!control.source.request_stop());
        ++control.request_returned;
    });
    control.terminal_returned.wait();
    CHECK(control.raw_destroyed == 1 && control.callback_returning == 0);
    CHECK(control.request_returned == 0 && control.stopped == 0);

    std::thread worker([&] {
        control.scheduler_thread = std::this_thread::get_id();
        control.loop.run();
    });
    control.completed.wait();
    worker.join();
    operation.reset();
    // Full outer storage is gone while request_stop and its callback stack are
    // still active. Neither callback tail nor library self-removal may touch it.
    CHECK(control.callback_returning == 0 && control.request_returned == 0);
    CHECK(control.raw_destroyed == 1 && control.stopped == 1);
    control.release_callback_tail.count_down();
    requester.join();
    CHECK(control.callback_thread != control.completion_thread);
    check_finished(control);
    // In particular the stop source outlives the original request_stop stack.
}

// Gate the real run_loop schedule's initiating start after enqueueing, so the
// outer operation can complete and be destroyed before the original callback's
// terminal CPO returns. All test coordination storage outlives both threads.
struct InflightControl : Control {
    std::latch schedule_started{1};
    std::latch release_schedule_start{1};
    std::atomic<int> outer_destroyed{0};
    bool synchronous_destruction = false;
    void* outer = nullptr;
    void (*destroy_outer)(InflightControl*) noexcept = nullptr;
};

template<class Inner>
struct StartTailGate {
    using sender_concept = stdexec::sender_tag;
    Inner inner;
    InflightControl* control;

    template<class, class Env>
    static consteval auto get_completion_signatures() noexcept
    { return stdexec::completion_signatures_of_t<Inner, Env>{}; }

    template<class Receiver>
    struct Operation {
        InflightControl* control;
        stdexec::connect_result_t<Inner, Receiver> inner;

        Operation(Inner sender, Receiver receiver, InflightControl* current)
            : control(current), inner(stdexec::connect(std::move(sender), std::move(receiver))) {}

        void start() & noexcept {
            auto* local = control;
            stdexec::start(inner);
            local->schedule_started.count_down();
            local->release_schedule_start.wait();
            // This gate, its inner schedule and the complete enclosing let chain
            // have been destroyed. Do not access any operation member here.
            CHECK(local->outer_destroyed == 1);
        }
    };

    template<class Receiver>
    auto connect(Receiver receiver) && {
        return Operation<Receiver>{std::move(inner), std::move(receiver), control};
    }
};

struct InflightReceiver {
    using receiver_concept = stdexec::receiver_tag;
    InflightControl* control;
    auto get_env() const noexcept {
        return stdexec::env{
            stdexec::prop{stdexec::get_stop_token, control->source.get_token()},
            stdexec::prop{stdexec::get_start_scheduler, control->loop.get_scheduler()}};
    }
    void set_stopped() noexcept {
        auto* local = control;
        CHECK(local->raw_destroyed == 1 && local->after_terminal == 0);
        CHECK(local->callback_returning == 0 && local->request_returned == 0);
        CHECK(++local->stopped == 1);
        local->completion_thread = std::this_thread::get_id();
        local->loop.finish();
        if (local->synchronous_destruction) local->destroy_outer(local);
        local->completed.count_down();
        // This receiver may already have been destroyed above or by coordinator.
    }
    template<class Error>
    void set_error(Error&&) noexcept { CHECK(false); }
};

auto inflight_sender(InflightControl& control) {
    return stdexec::let_stopped(CallbackSender{&control}, [local = &control] {
        return stdexec::let_value(stdexec::read_env(stdexec::get_start_scheduler),
            [local](auto scheduler) {
                ++local->schedules;
                return stdexec::let_value(
                    StartTailGate{stdexec::unstoppable(stdexec::schedule(scheduler)), local},
                    [local] {
                        CHECK(local->source.stop_requested());
                        ++local->schedule_values;
                        return stdexec::just_stopped();
                    });
            });
    });
}

void terminal_inflight(bool synchronous_destruction) {
    InflightControl control;
    control.synchronous_destruction = synchronous_destruction;
    using Operation = decltype(stdexec::connect(inflight_sender(control), InflightReceiver{&control}));
    control.outer = new Operation(stdexec::connect(inflight_sender(control), InflightReceiver{&control}));
    control.destroy_outer = [](InflightControl* local) noexcept {
        delete static_cast<Operation*>(local->outer);
        local->outer = nullptr;
        ++local->outer_destroyed;
    };
    stdexec::start(*static_cast<Operation*>(control.outer));
    std::thread requester([&] {
        CHECK(!control.source.request_stop());
        ++control.request_returned;
    });
    control.schedule_started.wait();
    CHECK(control.raw_destroyed == 1 && control.after_terminal == 0);
    std::thread worker([&] {
        control.scheduler_thread = std::this_thread::get_id();
        control.loop.run();
    });
    control.completed.wait();
    if (!synchronous_destruction) control.destroy_outer(&control);
    CHECK(control.outer_destroyed == 1 && control.request_returned == 0);
    CHECK(control.after_terminal == 0 && control.callback_returning == 0);
    control.release_schedule_start.count_down();
    requester.join();
    worker.join();
    check_finished(control);
}

int iterations(int argc, char** argv)
{
    if (argc == 1) return 100;
    CHECK(argc == 3 && std::string_view(argv[1]) == "--iterations");
    std::string_view text(argv[2]);
    int count = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    CHECK(error == std::errc{} && end == text.data() + text.size());
    CHECK(count > 0 && count <= 100000);
    return count;
}

} // namespace

int main(int argc, char** argv)
{
    const int count = iterations(argc, argv);
    for (int i = 0; i != count; ++i) {
        callback_returns_before_host_runs();
        outer_destroyed_before_callback_returns();
        terminal_inflight(false);
        terminal_inflight(true);
    }
    std::cout << "PASS: 4 deterministic callback/let_stopped/affinity lifetime cases x " << count
              << " iterations (pinned dependency composition probe only)\n";
    return 0;
}
