#pragma once

#include "pause.hpp"

#include <atomic>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <latch>
#include <memory>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace pause_sender_test {

[[noreturn]] inline void fail(const char* expression, int line) noexcept
{
    std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
    std::exit(EXIT_FAILURE);
}
#define CHECK(expression) ((expression) ? (void)0 : ::pause_sender_test::fail(#expression, __LINE__))

struct Gate {
    std::latch entered{1}, release{1};
    void hold() noexcept { entered.count_down(); release.wait(); }
};

struct SchedulerState {
    stdexec::run_loop loop;
    std::atomic<int> calls{0}, starts{0}, values{0}, stops{0};
    std::atomic<int> original_tokens{0}, unstoppable_tokens{0};
    stdexec::inplace_stop_token expected_token;
    std::thread::id thread;
    Gate* after_first_value = nullptr;
    Gate* after_stopped = nullptr;
    void* schedule_context = nullptr;
    void (*before_schedule)(SchedulerState*) noexcept = nullptr;
};

struct CountingScheduleSender;
struct CountingScheduler {
    using scheduler_concept = stdexec::scheduler_tag;
    SchedulerState* state;
    bool operator==(const CountingScheduler&) const noexcept = default;
    auto query(stdexec::get_completion_scheduler_t<stdexec::set_value_t>) const noexcept
    { return *this; }
    auto schedule() const noexcept -> CountingScheduleSender;
};

// A transparent test adapter around the pinned, approved run_loop backend.
// Unlike a synthetic queue it preserves the actual backend cancellation check.
struct CountingScheduleSender {
    using sender_concept = stdexec::sender_tag;
    using BaseSender = stdexec::schedule_result_t<stdexec::run_loop::scheduler>;
    SchedulerState* state;
    struct Attrs {
        SchedulerState* state;
        template<class Tag>
            requires (std::is_same_v<Tag, stdexec::set_value_t>
                      || std::is_same_v<Tag, stdexec::set_stopped_t>)
        auto query(stdexec::get_completion_scheduler_t<Tag>) const noexcept
        { return CountingScheduler{state}; }
        template<class Tag>
        static constexpr auto query(exec::get_completion_behavior_t<Tag>) noexcept
        { return exec::completion_behavior::asynchronous; }
    };
    auto get_env() const noexcept { return Attrs{state}; }
    template<class, class Env>
    static consteval auto get_completion_signatures() noexcept
    { return stdexec::completion_signatures_of_t<BaseSender, Env>{}; }
    template<class R>
    struct Receiver {
        using receiver_concept = stdexec::receiver_tag;
        SchedulerState* state;
        R receiver;
        auto get_env() const noexcept { return stdexec::get_env(receiver); }
        void set_value() noexcept {
            auto* gate = state->after_first_value;
            const bool first = ++state->values == 1;
            stdexec::set_value(std::move(receiver));
            if (first && gate) gate->hold();
        }
        void set_stopped() noexcept {
            auto* gate = state->after_stopped;
            ++state->stops;
            stdexec::set_stopped(std::move(receiver));
            if (gate) gate->hold();
        }
    };
    template<class R>
    struct Operation {
        SchedulerState* state;
        stdexec::connect_result_t<BaseSender, Receiver<R>> inner;
        Operation(SchedulerState* current, R receiver) noexcept
            : state(current), inner(stdexec::connect(
                  stdexec::schedule(current->loop.get_scheduler()),
                  Receiver<R>{current, std::move(receiver)}))
        {
            // Task erasure uses an inplace token bridge; the raw receiver path
            // below additionally checks equality with the public original token.
        }
        void start() & noexcept { ++state->starts; stdexec::start(inner); }
    };
    template<class R>
    auto connect(R receiver) const noexcept {
        const auto token = stdexec::get_stop_token(stdexec::get_env(receiver));
        if constexpr (std::same_as<decltype(token), const stdexec::inplace_stop_token>) {
            if (token == state->expected_token) ++state->original_tokens;
        }
        if constexpr (stdexec::unstoppable_token<decltype(token)>) ++state->unstoppable_tokens;
        return Operation<R>{state, std::move(receiver)};
    }
};
inline auto CountingScheduler::schedule() const noexcept -> CountingScheduleSender
{
    ++state->calls;
    if (state->before_schedule) state->before_schedule(state);
    return {state};
}
static_assert(stdexec::scheduler<CountingScheduler>);

enum class Completion { None, Value, Stopped };
struct ReceiverState {
    SchedulerState* scheduler;
    stdexec::inplace_stop_token token;
    std::atomic<Completion> result{Completion::None};
    std::latch completed{1};
    std::thread::id completion_thread;
    bool finish_loop = true;
    bool start_active = false;
    bool completed_in_start = false;
    void* operation = nullptr;
    void (*on_completion)(ReceiverState*) noexcept = nullptr;
    Gate* completion_tail = nullptr;
};
struct Receiver {
    using receiver_concept = stdexec::receiver_tag;
    ReceiverState* state;
    auto get_env() const noexcept {
        return stdexec::env{
            stdexec::prop{stdexec::get_stop_token, state->token},
            stdexec::prop{stdexec::get_start_scheduler, CountingScheduler{state->scheduler}}};
    }
    void complete(Completion result) noexcept {
        auto* local = state;
        CHECK(local->result.exchange(result) == Completion::None);
        local->completion_thread = std::this_thread::get_id();
        local->completed_in_start = local->start_active;
        auto* tail = local->completion_tail;
        if (local->on_completion) local->on_completion(local);
        if (local->finish_loop) local->scheduler->loop.finish();
        local->completed.count_down();
        if (tail) tail->hold();
    }
    void set_value() noexcept { complete(Completion::Value); }
    void set_stopped() noexcept { complete(Completion::Stopped); }
    template<class Error> void set_error(Error&&) noexcept { CHECK(false); }
};
struct IgnoreReceiver {
    using receiver_concept = stdexec::receiver_tag;
    void set_value() noexcept {}
    void set_stopped() noexcept { CHECK(false); }
    template<class Error> void set_error(Error&&) noexcept { CHECK(false); }
};
template<class F>
void run_in_context(SchedulerState& scheduler, F function) {
    auto entry = stdexec::connect(stdexec::then(
        stdexec::schedule(scheduler.loop.get_scheduler()), std::move(function)), IgnoreReceiver{});
    stdexec::start(entry);
    scheduler.thread = std::this_thread::get_id();
    scheduler.loop.run();
}
inline int iterations(int argc, char** argv) {
    if (argc == 1) return 100;
    CHECK(argc == 3 && std::string_view(argv[1]) == "--iterations");
    std::string_view text(argv[2]);
    int count = 0;
    auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), count);
    CHECK(error == std::errc{} && end == text.data() + text.size());
    CHECK(count > 0 && count <= 1000000);
    return count;
}

} // namespace pause_sender_test
