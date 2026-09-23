#include <stdexec/stop_token.hpp>

#include <atomic>
#include <barrier>
#include <charconv>
#include <cstdlib>
#include <iostream>
#include <latch>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    [[noreturn]] void fail(const char* expression, int line)
    {
        std::cerr << "CHECK failed at line " << line << ": " << expression << '\n';
        std::exit(EXIT_FAILURE);
    }

#define CHECK(expression) ((expression) ? (void)0 : fail(#expression, __LINE__))

    // These are dependency-contract tests, not tests of a Pause implementation.
    // The pinned stdexec revision returns false for the first request_stop(),
    // and true for subsequent requests (unlike std::stop_source).
    struct callback_function
    {
        void* context;
        void (*function)(void*) noexcept;

        void operator()() const noexcept
        {
            // A callback may destroy its own registration. Do not access this
            // after invoking the function.
            auto* local_context = context;
            auto* local_function = function;
            local_function(local_context);
        }
    };

    using registration = stdexec::inplace_stop_callback<callback_function>;

    void empty_token()
    {
        int calls = 0;
        stdexec::inplace_stop_token token;
        CHECK(!token.stop_possible());
        CHECK(!token.stop_requested());
        {
            registration callback(token, callback_function{&calls, [](void* raw) noexcept {
                                      ++*static_cast<int*>(raw);
                                  }});
            CHECK(calls == 0);
        }
        CHECK(calls == 0);
    }

    void already_stopped_constructor()
    {
        stdexec::inplace_stop_source source;
        CHECK(!source.request_stop());
        struct state
        {
            bool constructor_returned = false;
            int calls = 0;
            std::thread::id constructing_thread = std::this_thread::get_id();
        } state;
        registration callback(source.get_token(), callback_function{&state, [](void* raw) noexcept {
                                  auto& current = *static_cast<struct state*>(raw);
                                  CHECK(!current.constructor_returned);
                                  CHECK(current.constructing_thread == std::this_thread::get_id());
                                  ++current.calls;
                              }});
        state.constructor_returned = true;
        CHECK(state.calls == 1);
        CHECK(source.request_stop());
        CHECK(state.calls == 1);
    }

    void unregister_before_stop()
    {
        stdexec::inplace_stop_source source;
        int calls = 0;
        {
            registration callback(source.get_token(), callback_function{&calls, [](void* raw) noexcept {
                                      ++*static_cast<int*>(raw);
                                  }});
        }
        CHECK(!source.request_stop());
        CHECK(source.request_stop());
        CHECK(calls == 0);
    }

    void repeated_and_reentrant_stop()
    {
        stdexec::inplace_stop_source source;
        struct state
        {
            stdexec::inplace_stop_source* source;
            int calls = 0;
        } state{&source};
        registration callback(source.get_token(), callback_function{&state, [](void* raw) noexcept {
                                  auto& current = *static_cast<struct state*>(raw);
                                  ++current.calls;
                                  CHECK(current.source->request_stop());
                              }});
        CHECK(!source.request_stop());
        for (int i = 0; i != 10; ++i)
            CHECK(source.request_stop());
        CHECK(state.calls == 1);
    }

    void concurrent_requesters()
    {
        stdexec::inplace_stop_source source;
        std::atomic<int> calls = 0;
        std::atomic<int> first_requests = 0;
        registration callback(source.get_token(), callback_function{&calls, [](void* raw) noexcept {
                                  static_cast<std::atomic<int>*>(raw)->fetch_add(1);
                              }});
        constexpr int thread_count = 4;
        std::barrier start(thread_count);
        std::vector<std::thread> requesters;
        for (int i = 0; i != thread_count; ++i)
        {
            requesters.emplace_back([&] {
                start.arrive_and_wait();
                if (!source.request_stop())
                    first_requests.fetch_add(1);
            });
        }
        for (auto& requester : requesters)
            requester.join();
        CHECK(calls.load() == 1);
        CHECK(first_requests.load() == 1);
    }

    void registration_races_stop()
    {
        stdexec::inplace_stop_source source;
        std::atomic<int> calls = 0;
        std::barrier start(2);
        std::latch request_returned(1);
        std::thread constructor([&] {
            start.arrive_and_wait();
            registration callback(source.get_token(), callback_function{&calls, [](void* raw) noexcept {
                                      static_cast<std::atomic<int>*>(raw)->fetch_add(1);
                                  }});
            // Retain the registration until stop has been requested, regardless
            // of whether registration or request_stop wins the initial race.
            request_returned.wait();
        });
        std::thread requester([&] {
            start.arrive_and_wait();
            CHECK(!source.request_stop());
            request_returned.count_down();
        });
        constructor.join();
        requester.join();
        CHECK(calls.load() == 1);
        // Public APIs cannot force the narrow interval between linking the
        // callback and the constructor returning. This case races that interval
        // but does not claim deterministic coverage of it.
    }

    struct reset_order_state
    {
        std::latch callback_entered{1};
        std::latch reset_call_started{1};
        std::atomic<int> sequence = 0;
        int callback_tail_order = 0;
        int functor_destroyed_order = 0;
        int reset_returned_order = 0;
    };

    struct reset_order_function
    {
        reset_order_state* state;

        explicit reset_order_function(reset_order_state* current) noexcept : state(current) {}
        reset_order_function(reset_order_function&& other) noexcept
            : state(std::exchange(other.state, nullptr))
        {}

        ~reset_order_function()
        {
            if (state != nullptr)
                state->functor_destroyed_order = state->sequence.fetch_add(1) + 1;
        }

        void operator()() const noexcept
        {
            auto* local_state = state;
            local_state->callback_entered.count_down();
            local_state->reset_call_started.wait();
            local_state->callback_tail_order = local_state->sequence.fetch_add(1) + 1;
        }
    };

    void cross_thread_unregister_order()
    {
        stdexec::inplace_stop_source source;
        reset_order_state state;
        using ordered_registration = stdexec::inplace_stop_callback<reset_order_function>;
        auto callback = std::make_unique<ordered_registration>(source.get_token(),
                                                               reset_order_function(&state));
        std::thread requester([&] { CHECK(!source.request_stop()); });
        state.callback_entered.wait();
        // The callback is executing before this thread begins unregistering.
        // This establishes callback-tail < functor-destruction < reset-return.
        // It cannot force a thread to pause inside the library's destructor;
        // there is intentionally no instantaneous "has not returned" check.
        state.reset_call_started.count_down();
        callback.reset();
        state.reset_returned_order = state.sequence.fetch_add(1) + 1;
        requester.join();
        CHECK(state.callback_tail_order == 1);
        CHECK(state.functor_destroyed_order == 2);
        CHECK(state.reset_returned_order == 3);
    }

    void same_thread_self_destruction()
    {
        stdexec::inplace_stop_source source;
        struct state
        {
            std::unique_ptr<registration> callback;
            int calls = 0;
            bool continued_after_reset = false;
        } state;
        state.callback = std::make_unique<registration>(
            source.get_token(), callback_function{&state, [](void* raw) noexcept {
                auto* current = static_cast<struct state*>(raw);
                ++current->calls;
                current->callback.reset();
                current->continued_after_reset = true;
            }});
        CHECK(!source.request_stop());
        CHECK(!state.callback);
        CHECK(state.calls == 1);
        CHECK(state.continued_after_reset);
        CHECK(source.request_stop());
    }

    void second_request_returns_while_first_is_active()
    {
        stdexec::inplace_stop_source source;
        struct state
        {
            std::latch callback_entered{1};
            std::latch allow_callback_return{1};
            bool second_request_returned = false;
            bool observed_second_return = false;
        } state;
        registration callback(source.get_token(), callback_function{&state, [](void* raw) noexcept {
                                  auto& current = *static_cast<struct state*>(raw);
                                  current.callback_entered.count_down();
                                  current.allow_callback_return.wait();
                                  current.observed_second_return = current.second_request_returned;
                              }});
        std::thread first_request([&] { CHECK(!source.request_stop()); });
        state.callback_entered.wait();
        CHECK(source.request_stop());
        state.second_request_returned = true;
        // The first request cannot return before this gate opens; the second
        // request has already returned. No timing or scheduling assumption.
        state.allow_callback_return.count_down();
        first_request.join();
        CHECK(state.observed_second_return);
    }

    // A deliberately small ownership probe for the design's claim/unregister
    // protocol. It is not Pause, and does not model pending counts or branches.
    struct claim_probe
    {
        enum class phase { waiting, value_claimed, stop_claimed, completed };

        std::mutex mutex;
        phase current = phase::waiting;
        std::unique_ptr<registration> callback;
        int value_completions = 0;
        int stopped_completions = 0;
        std::atomic<int> callback_calls = 0;
        std::latch* callback_entered = nullptr;
        std::latch* stop_claimed = nullptr;
        std::latch* allow_stop_completion = nullptr;

        bool claim(phase desired)
        {
            std::lock_guard lock(mutex);
            if (current != phase::waiting)
                return false;
            current = desired;
            return true;
        }

        void complete(phase owner)
        {
            std::lock_guard lock(mutex);
            CHECK(current == owner);
            CHECK(!callback);
            current = phase::completed;
            if (owner == phase::value_claimed)
                ++value_completions;
            else
                ++stopped_completions;
        }

        void cancel() noexcept
        {
            callback_calls.fetch_add(1);
            if (callback_entered != nullptr)
                callback_entered->count_down();
            if (!claim(phase::stop_claimed))
                return;
            if (stop_claimed != nullptr)
                stop_claimed->count_down();
            if (allow_stop_completion != nullptr)
                allow_stop_completion->wait();
            callback.reset();
            complete(phase::stop_claimed);
        }

        void resume()
        {
            if (!claim(phase::value_claimed))
                return;
            // Reset outside mutex: an executing callback may need the mutex.
            callback.reset();
            complete(phase::value_claimed);
        }

        void install(stdexec::inplace_stop_token token)
        {
            callback = std::make_unique<registration>(
                token, callback_function{this, [](void* raw) noexcept {
                    static_cast<claim_probe*>(raw)->cancel();
                }});
        }

        void check_completed()
        {
            CHECK(current == phase::completed);
            CHECK(!callback);
            CHECK(value_completions + stopped_completions == 1);
            CHECK(callback_calls.load() <= 1);
        }
    };

    void resume_claims_before_callback()
    {
        stdexec::inplace_stop_source source;
        claim_probe probe;
        std::latch entered(1);
        probe.callback_entered = &entered;
        probe.install(source.get_token());
        std::unique_lock lock(probe.mutex);
        probe.current = claim_probe::phase::value_claimed;
        std::thread requester([&] { CHECK(!source.request_stop()); });
        entered.wait();
        // The callback has entered, while Resume still owns the state mutex.
        // Unregister only after releasing that mutex.
        lock.unlock();
        probe.callback.reset();
        probe.complete(claim_probe::phase::value_claimed);
        requester.join();
        probe.check_completed();
        CHECK(probe.value_completions == 1);
        CHECK(probe.callback_calls.load() == 1);
    }

    void callback_claims_before_resume()
    {
        stdexec::inplace_stop_source source;
        claim_probe probe;
        std::latch claimed(1);
        std::latch allow_completion(1);
        probe.stop_claimed = &claimed;
        probe.allow_stop_completion = &allow_completion;
        probe.install(source.get_token());
        std::thread requester([&] { CHECK(!source.request_stop()); });
        claimed.wait();
        probe.resume();
        allow_completion.count_down();
        requester.join();
        probe.check_completed();
        CHECK(probe.stopped_completions == 1);
        CHECK(probe.callback_calls.load() == 1);
    }

    void resume_races_callback()
    {
        stdexec::inplace_stop_source source;
        claim_probe probe;
        probe.install(source.get_token());
        std::barrier start(2);
        std::thread requester([&] {
            start.arrive_and_wait();
            CHECK(!source.request_stop());
        });
        std::thread resumer([&] {
            start.arrive_and_wait();
            probe.resume();
        });
        requester.join();
        resumer.join();
        probe.check_completed();
    }

    int parse_iterations(int argc, char** argv)
    {
        if (argc == 1)
            return 200;
        if (argc != 3 || std::string_view(argv[1]) != "--iterations")
        {
            std::cerr << "usage: stdexec_stop_callback_test [--iterations 1..100000]\n";
            std::exit(EXIT_FAILURE);
        }
        std::string_view argument(argv[2]);
        int iterations = 0;
        auto [end, error] = std::from_chars(argument.data(), argument.data() + argument.size(), iterations);
        if (error != std::errc{} || end != argument.data() + argument.size()
            || iterations < 1 || iterations > 100000)
        {
            std::cerr << "invalid iteration count\n";
            std::exit(EXIT_FAILURE);
        }
        return iterations;
    }
}

int main(int argc, char** argv)
{
    const int iterations = parse_iterations(argc, argv);
    empty_token();
    already_stopped_constructor();
    unregister_before_stop();
    repeated_and_reentrant_stop();
    same_thread_self_destruction();
    for (int i = 0; i != iterations; ++i)
    {
        concurrent_requesters();
        registration_races_stop();
        cross_thread_unregister_order();
        second_request_returns_while_first_is_active();
        resume_claims_before_callback();
        callback_claims_before_resume();
        resume_races_callback();
    }
    std::cout << "PASS: 12 stop-callback cases; 7 concurrent cases x " << iterations
              << " iterations (dependency and ownership-probe tests only)\n";
}
